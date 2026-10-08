/*
 * hook_libc.cpp — emulate sdcardfs mode handling in userspace.
 *
 * Raw /data/media access uses a POSIX ACL named entry for gid 9997
 * (AID_EVERYBODY). ACLs are not virtualisation: the kernel applies them to the
 * requested mode, so 0600 create/chmod zeroes the mask (posix_acl_create_masq)
 * and the entry dies -> EACCES. sdcardfs instead synthesises 0770 dirs / 0660
 * files, gid 9997 (mask=0007). Patch bionic entry points to shape the mode
 * pre-syscall: open/openat group rw + other cleared; mkdirat also group x;
 * fchmod/fchmodat never narrowed; renameat/renameat2/linkat into storage adds the
 * ACL (rename skips the default ACL). Only roots are patched; a thunk is skipped,
 * and its calls reach the root through .plt, so creat, mkdir, chmod, rename, link
 * and the mkstemp family need no entry of their own. Entry patching covers
 * loaded/later-dlopen'd/dlsym'd calls and needs no trampoline (handlers syscall
 * directly); handlers must be reentrant (syscalls only). arm64 only.
 *
 * The patch is 20 bytes and opens with bti jc, so a patched entry stays a legal
 * branch target on a bionic built with -mbranch-protection (17 already is), and
 * hooks_install() refuses the release when libc declares BTI but the module was
 * built without it. See patch_entry for why bti jc and x17 specifically.
 *
 * kHooks[] lists the roots, renameat among them — a root of its own on
 * 11/12/12L/13, and the only way rename() is covered there. The names it does NOT
 * list (creat, creat64, mkdir, chmod, rename, link, mkstemp, mkostemp, mkstemps,
 * mkostemps) are absent because they are tail branches onto a root on every one of
 * the eight validated releases: the engine would resolve each, classify it, and skip
 * it, so listing one would add a name that is never patched. (Which of the two skip
 * labels such a name gets depends only on its length against the 20-byte gate —
 * creat at 12 bytes reads "коротка", chmod at 20 reads "переходник" — and neither is
 * a patch.) That coverage is not
 * assumed — hookselftest.cpp creates through creat/mkstemp/mkstemps and renames
 * through rename, then checks the mode that comes out, and
 * tools/verify-hook-targets.py unwinds the same chains statically. android_ver.h
 * names the releases and what each covers; the COUNT is not something to assume,
 * and hooks_release() reports a release that comes out otherwise.
 *
 *   tools/verify-hook-targets.py device/libc/libc-arm64.so
 */

#include "hook_libc.h"

#include "android_ver.h"
#include "gnu_props.h"

#include "func_size.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <dlfcn.h>
#include <link.h>  // dl_iterate_phdr, ElfW: the branch-protection preflight

#include <android/log.h>

#define LOG_TAG "UnfuseZygisk"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// AID_EVERYBODY: shared group of all apps in a profile (sdcardfs mounts).
constexpr uint32_t kAidEverybody = 9997;

// A thunk is never longer. arm64 only (see tail_call_target).
[[maybe_unused]] constexpr unsigned kThunkMax = 32;

// POSIX ACL xattr names
constexpr const char *kAclAccess = "system.posix_acl_access";
constexpr const char *kAclDefault = "system.posix_acl_default";

// linux/include/uapi/linux/posix_acl_xattr.h
constexpr uint16_t kAclUserObj = 0x01;
constexpr uint16_t kAclGroupObj = 0x04;
constexpr uint16_t kAclGroup = 0x08;
constexpr uint16_t kAclMask = 0x10;
constexpr uint16_t kAclOther = 0x20;
constexpr uint32_t kAclVersion = 0x0002;

struct acl_entry {
    uint16_t e_tag;
    uint16_t e_perm;
    uint32_t e_id;
};

// Shared storage: /storage/ (emulated/self/removable), /mnt/user and /mnt/runtime
// (same tree pre-Zygote bind), /mnt/pass_through (AOSP FUSE), /data/media (raw).
constexpr const char *kStoragePrefixes[] = {
    "/storage/",
    "/mnt/user/",
    "/mnt/runtime/",
    "/mnt/pass_through/",
    "/data/media/",
};

// /sdcard -> /storage/self/primary; whole component, so "/sdcardfoo" does not match.
bool sdcard_prefix(const char *path) {
    static constexpr char k[] = "/sdcard";
    if (strncmp(path, k, sizeof(k) - 1) != 0) return false;
    const char c = path[sizeof(k) - 1];
    return c == '\0' || c == '/';
}

bool absolute_is_storage(const char *path) {
    for (const char *p : kStoragePrefixes) {
        if (strncmp(path, p, strlen(p)) == 0) return true;
    }
    return sdcard_prefix(path);
}

// "/proc/self/fd/<n>" without snprintf (no stdio in handlers).
void fd_link_path(int fd, char *out, size_t len) {
    static constexpr char kPfx[] = "/proc/self/fd/";
    size_t i = 0;
    while (i < sizeof(kPfx) - 1 && i + 1 < len) out[i] = kPfx[i], i++;

    char num[12];
    int n = 0;
    if (fd == 0) {
        num[n++] = '0';
    }
    for (int v = fd; v > 0 && n < (int)sizeof(num); v /= 10) num[n++] = (char)('0' + v % 10);
    while (n > 0 && i + 1 < len) out[i++] = num[--n];
    out[i] = '\0';
}

// fd on shared storage? For fchmod/fchmodat.
bool fd_is_storage(int fd) {
    char link[32];
    fd_link_path(fd, link, sizeof link);
    char target[512];
    const long n = syscall(SYS_readlinkat, AT_FDCWD, link, target, sizeof target - 1);
    if (n <= 0) return false;
    target[n] = '\0';
    return absolute_is_storage(target);
}

bool is_storage(int dirfd, const char *path) {
    if (path == nullptr) return false;
    if (path[0] == '/') return absolute_is_storage(path);
    if (dirfd != AT_FDCWD) return fd_is_storage(dirfd);

    // Relative to cwd; rare but native chdir() exists, so never guess wrong.
    char cwd[512];
    const long n = syscall(SYS_getcwd, cwd, sizeof cwd);
    if (n <= 0) return false;
    return absolute_is_storage(cwd);
}

// sdcardfs view: owner kept, group rw (dirs +x), other cleared by mask 0007
// (0666 reads as 0660).
mode_t as_sdcardfs_file(mode_t m) {
    return (m & S_IRWXU) | (m & S_IRWXG) | S_IRGRP | S_IWGRP;
}

mode_t as_sdcardfs_dir(mode_t m) {
    return (m & S_IRWXU) | (m & S_IRWXG) | S_IRGRP | S_IWGRP | S_IXGRP;
}

// chmod needs different group bits for dir vs file.
mode_t widen_existing(int dirfd, const char *path, mode_t mode, int at_flags) {
    struct stat st;
    if (fstatat(dirfd, path, &st, at_flags) == 0 && S_ISDIR(st.st_mode)) {
        return as_sdcardfs_dir(mode);
    }
    return as_sdcardfs_file(mode);
}

// Five entries as vold::SetDefaultAcl (vold-16/Utils.cpp:142) / tools/storage-fix.c.
// The named 9997 entry and the mask take the group perms (else the mask revokes
// the access); OTHER is always cleared, matching sdcardfs mask 0007.
void acl_build(uint8_t *buf, size_t *len, mode_t mode) {
    const uint16_t g = static_cast<uint16_t>((mode & S_IRWXG) >> 3);

    acl_entry e[5];
    e[0] = {kAclUserObj, static_cast<uint16_t>((mode & S_IRWXU) >> 6), static_cast<uint32_t>(-1)};
    e[1] = {kAclGroupObj, g, static_cast<uint32_t>(-1)};
    e[2] = {kAclGroup, g, kAidEverybody};
    e[3] = {kAclMask, g, 0};
    e[4] = {kAclOther, 0, 0};

    memcpy(buf, &kAclVersion, sizeof(uint32_t));
    memcpy(buf + sizeof(uint32_t), e, sizeof(e));
    *len = sizeof(uint32_t) + sizeof(e);
}

int acl_write(const char *path, const char *name, mode_t mode) {
    uint8_t buf[sizeof(uint32_t) + 5 * sizeof(acl_entry)];
    size_t len = 0;
    acl_build(buf, &len, mode);
    return static_cast<int>(syscall(SYS_setxattr, path, name, buf, len, 0));
}

int acl_write_fd(int fd, const char *name, mode_t mode) {
    uint8_t buf[sizeof(uint32_t) + 5 * sizeof(acl_entry)];
    size_t len = 0;
    acl_build(buf, &len, mode);
    return static_cast<int>(syscall(SYS_fsetxattr, fd, name, buf, len, 0));
}

// Mode first (it defines the ACL mask), then ACL: chmod rewrites
// USER_OBJ/GROUP_OBJ/MASK/OTHER, so the ACL must land last. Preferred when an fd
// exists (no cwd/path-race dependence).
void fix_fd(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return;
    if (S_ISLNK(st.st_mode)) return;
    const bool dir = S_ISDIR(st.st_mode);
    if (!dir && !S_ISREG(st.st_mode)) return;

    const mode_t want = dir ? as_sdcardfs_dir(st.st_mode) : as_sdcardfs_file(st.st_mode);
    syscall(SYS_fchmod, fd, want);
    acl_write_fd(fd, kAclAccess, want);
    if (dir) acl_write_fd(fd, kAclDefault, want);
}

// Same by path, for rename/link (no fd).
void fix_object(const char *path) {
    struct stat st;
    if (fstatat(AT_FDCWD, path, &st, AT_SYMLINK_NOFOLLOW) != 0) return;
    if (S_ISLNK(st.st_mode)) return;
    const bool dir = S_ISDIR(st.st_mode);
    if (!dir && !S_ISREG(st.st_mode)) return;

    const mode_t want = dir ? as_sdcardfs_dir(st.st_mode) : as_sdcardfs_file(st.st_mode);

    syscall(SYS_fchmodat, AT_FDCWD, path, want, 0);
    acl_write(path, kAclAccess, want);
    if (dir) acl_write(path, kAclDefault, want);
}

// New dir, no fd: open it and fix via fd (no absolute path or /proc).
void fix_created_dir(int dirfd, const char *path) {
    const int fd = static_cast<int>(
        syscall(SYS_openat, dirfd, path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0));
    if (fd < 0) return;
    fix_fd(fd);
    syscall(SYS_close, fd);
}

// Create vs open-existing: only a new object gets an ACL.
bool exists_at(int dirfd, const char *path) {
    struct stat st;
    return fstatat(dirfd, path, &st, 0) == 0;
}

// Handlers replace the original and syscall directly: independent of patch order
// and of the call going through the PLT.

bool needs_mode(int flags) {
    return (flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE;
}

// "Open with create": shape mode, syscall, ACL if new. A mode alone is not
// enough: cross-app access needs the named 9997 ACL entry inherited from the dir
// default ACL — fragile, since vold rebuilds /data/media/<user> and
// Android/{data,obb,media} each boot and module/vold order is not guaranteed (on
// device /data/media/0 default ACL was 1023, not 9997, hiding new root files).
int open_and_fix(int dirfd, const char *path, int flags, mode_t mode) {
    const bool storage = needs_mode(flags) && is_storage(dirfd, path);
    const bool existed = storage && exists_at(dirfd, path);
    if (storage) mode = as_sdcardfs_file(mode);

    const int fd = static_cast<int>(syscall(SYS_openat, dirfd, path, flags, mode));
    if (fd >= 0 && storage && !existed) fix_fd(fd);
    return fd;
}

extern "C" int h_open(const char *path, int flags, mode_t mode) {
    return open_and_fix(AT_FDCWD, path, flags, mode);
}

extern "C" int h_openat(int dirfd, const char *path, int flags, mode_t mode) {
    return open_and_fix(dirfd, path, flags, mode);
}

// Mode-less fortify variants. If O_CREAT still arrives (caller error; bionic
// aborts), use 0660, not 0, which would zero the ACL mask.
extern "C" int h_open_2(const char *path, int flags) {
    return open_and_fix(AT_FDCWD, path, flags, needs_mode(flags) ? 0666 : 0);
}

extern "C" int h_openat_2(int dirfd, const char *path, int flags) {
    return open_and_fix(dirfd, path, flags, needs_mode(flags) ? 0666 : 0);
}

extern "C" int h_mkdirat(int dirfd, const char *path, mode_t mode) {
    const bool storage = is_storage(dirfd, path);
    if (storage) mode = as_sdcardfs_dir(mode);

    const int r = static_cast<int>(syscall(SYS_mkdirat, dirfd, path, mode));
    if (r == 0 && storage) fix_created_dir(dirfd, path);
    return r;
}

extern "C" int h_fchmodat(int dirfd, const char *path, mode_t mode, int at_flags) {
    if (is_storage(dirfd, path)) mode = widen_existing(dirfd, path, mode, at_flags);
    return static_cast<int>(syscall(SYS_fchmodat, dirfd, path, mode, at_flags));
}

extern "C" int h_fchmod(int fd, mode_t mode) {
    if (fd_is_storage(fd)) {
        struct stat st;
        if (syscall(SYS_fstat, fd, &st) == 0) {
            mode = S_ISDIR(st.st_mode) ? as_sdcardfs_dir(mode) : as_sdcardfs_file(mode);
        }
    }
    return static_cast<int>(syscall(SYS_fchmod, fd, mode));
}

// rename/link fixed after the op, only when entering storage from outside:
// rename skips the default ACL, so a 0600 file from /data/data/<pkg> would land
// in /sdcard with no ACL.
void fix_after_move(int dirfd, const char *dst, int src_dirfd, const char *src) {
    if (dst == nullptr) return;
    if (!is_storage(dirfd, dst)) return;
    if (src != nullptr && is_storage(src_dirfd, src)) return;
    fix_object(dst);
}

extern "C" int h_renameat(int olddirfd, const char *oldp, int newdirfd, const char *newp) {
    const int r = static_cast<int>(syscall(SYS_renameat, olddirfd, oldp, newdirfd, newp));
    if (r == 0) fix_after_move(newdirfd, newp, olddirfd, oldp);
    return r;
}

extern "C" int h_renameat2(int olddirfd, const char *oldp, int newdirfd, const char *newp,
                           unsigned flags) {
    const int r = static_cast<int>(syscall(SYS_renameat2, olddirfd, oldp, newdirfd, newp, flags));
    if (r == 0) fix_after_move(newdirfd, newp, olddirfd, oldp);
    return r;
}

extern "C" int h_linkat(int olddirfd, const char *oldp, int newdirfd, const char *newp, int flags) {
    const int r = static_cast<int>(syscall(SYS_linkat, olddirfd, oldp, newdirfd, newp, flags));
    if (r == 0) fix_after_move(newdirfd, newp, olddirfd, oldp);
    return r;
}

// The mkstemp family (mkstemp, mkostemp, mkstemps, mkostemps) is NOT hooked, and
// needs no reimplementation. It does create 0600 — exactly what zeroes the ACL
// mask — but bionic reaches its own open through open@plt, so the patched open
// entry sees the 0600, widens it to 0660 and writes the ACL itself. A handler
// here would have to reimplement mkstemp (the original cannot be called once its
// entry is overwritten) purely to do what open already does. hookselftest.cpp
// checks this end to end on the device, through mkstemp and mkstemps.

#if defined(__aarch64__)

// 20 bytes: bti jc; ldr x17,#8; br x17; .quad <handler>. The literal is
// PC-relative (the load at offset 4 reads offset 12) and the branch is
// register-indirect, so the patch needs no range or instruction relocation — but
// it does need 20 bytes of room, hence the size gate in hooks_install.
//
// bti jc is not decoration. bionic 17 is built with -mbranch-protection=standard
// (roots open with paciasp, thunks with bti c), and a loader sets PROT_BTI as
// soon as an image declares GNU_PROPERTY_AARCH64_FEATURE_1_BTI. No Android image
// declares it today, so these are inert hints on 13/14/15/16/17 — but if one ever
// does, offset 0 becomes a guarded entry and an indirect call to a patched root
// must land on a landing pad. The pad has to come first: a patch starting with the
// load would fault before ever reaching the handler.
//
// jc rather than c: the pad has to accept both branch types the entry can see. A
// plain call arrives with BTYPE=call (bti c would do), but a jump that did not
// come through x16/x17 arrives with BTYPE=jump, which bti c rejects. jc accepts
// both, and costs the same one instruction.
//
// Two constraints follow, both silent if broken:
//   - paciasp, the other legal pad (and what bionic uses at framed entries), is
//     unusable here: it signs x30 against the caller's SP, so the handler's ret
//     would return to a signed address.
//   - the branch must stay in x17. A br normally requires a bti j pad, but the
//     architecture exempts x16/x17 exactly so the PLT idiom may land on bti c —
//     which is also why the branch cannot move to another register.
constexpr uint32_t kBitJc = 0xd50324dfu;   // bti jc (hint #38)
constexpr uint32_t kLdrX17 = 0x58000051u;  // ldr x17, #8
constexpr uint32_t kBrX17 = 0xd61f0220u;   // br  x17
constexpr size_t kPatchSize = 20;

bool patch_entry(void *target, void *handler) {
    const uintptr_t addr = reinterpret_cast<uintptr_t>(target);
    if ((addr & 3u) != 0) {
        LOGE("вход %p не выровнен по 4 байта", target);
        return false;
    }

    const long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) return false;
    const uintptr_t page = addr & ~(static_cast<uintptr_t>(ps) - 1);

    // Patch may straddle a page boundary; map both pages.
    const size_t span = static_cast<size_t>(addr - page) + kPatchSize;
    const size_t mlen = (span + static_cast<size_t>(ps) - 1) & ~(static_cast<size_t>(ps) - 1);

    // PROT_EXEC kept: .text already has VM_EXEC, so adding write is not creating
    // executable memory (else SELinux needs process execmem). Write lands in the
    // private page copy; the on-disk lib is unchanged.
    if (mprotect(reinterpret_cast<void *>(page), mlen,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("mprotect(%p, %zu) -> %s", reinterpret_cast<void *>(page), mlen, strerror(errno));
        return false;
    }

    // The write is not atomic, and no order of these stores is safe in general —
    // each order only picks the least bad window. This one is chosen so that:
    //   after the literal — the original code runs: correct for every target;
    //   after the load    — framed roots still run their own body (x17 is
    //                       call-clobbered scratch, so clobbering it is
    //                       harmless); the leaf syscall wrappers skip their
    //                       syscall and return a wrong value, but do not crash;
    //   after the branch  — the leaf wrappers work, but a framed root is now
    //                       entered through a live paciasp, which signs x30 and
    //                       breaks the handler's ret;
    //   after the pad     — final state.
    // The pad therefore goes last: the one broken window is a single store wide
    // and covers only the framed roots. What makes any of this acceptable is
    // that it runs from postAppSpecialize, where the process has one thread and
    // nothing can be executing the entry being rewritten.
    //
    // volatile: the order above is the whole argument, and it is only preserved
    // if the compiler is not free to reorder stores through these casts.
    auto *words = reinterpret_cast<volatile uint32_t *>(addr);
    auto *literal = reinterpret_cast<volatile uint64_t *>(addr + 12);
    *literal = reinterpret_cast<uint64_t>(handler);
    words[1] = kLdrX17;
    words[2] = kBrX17;
    words[0] = kBitJc;
    __builtin___clear_cache(reinterpret_cast<char *>(addr),
                            reinterpret_cast<char *>(addr) + kPatchSize);

    // Read back what was actually written. The pad is the one word whose absence
    // would be silent today and fatal the moment a release declares BTI, so it is
    // worth one load to know the page really took the write.
    const uint32_t got_pad = words[0];
    const uint32_t got_ldr = words[1];
    const uint32_t got_br = words[2];
    if (got_pad != kBitJc || got_ldr != kLdrX17 || got_br != kBrX17) {
        LOGE("патч входа %p не лёг: bti=%08x ldr=%08x br=%08x",
             target, got_pad, got_ldr, got_br);
        mprotect(reinterpret_cast<void *>(page), mlen, PROT_READ | PROT_EXEC);
        return false;
    }

    mprotect(reinterpret_cast<void *>(page), mlen, PROT_READ | PROT_EXEC);
    return true;
}

// Thunk = short body of arg shuffles + one tail branch to the real impl ->
// return the branch target, else nullptr.
//
// Short AND ends in an unconditional branch; neither alone suffices (a large
// function may tail-call; a short one may be the real impl). bionic thunks
// shuffle args first, branch last, so the first instruction proves nothing.
//
// A 12-byte thunk vs a 20-byte patch would clobber the next function, so no
// patch: the branch goes via .plt and bionic libc lacks -Bsymbolic, so
// intra-library calls use the GOT pointing at the already-patched entry
// (libc-16 R_AARCH64_JUMP_SLOT: creat/creat64 -> open@plt -> open; renameat ->
// renameat2@plt -> renameat2; mkstemps/mkostemps -> mktemp_internal -> open@plt).
//
// Safety is size: the patch needs >=20 bytes, so a long thunk (mkdir = 20 on 17)
// may be patched but need not be; tools/verify-hook-targets.py uses the same rule.
void *tail_call_target(const void *fn, unsigned size) {
    if (size < 4 || size > kThunkMax) return nullptr;

    const uint32_t insn = static_cast<const uint32_t *>(fn)[size / 4 - 1];

    // Match B (imm26), not BL: a call that returns is a normal body, not a tail
    // branch.
    if ((insn & 0xfc000000u) != 0x14000000u) return nullptr;

    // imm26 is a signed instruction-count offset (4 bytes each).
    int32_t off = static_cast<int32_t>(insn & 0x03ffffffu);
    if ((off & 0x02000000) != 0) off -= 0x04000000;

    return const_cast<uint32_t *>(static_cast<const uint32_t *>(fn)) + off;
}

#else

// Same width as the arm64 patch, so the size gate in hooks_install means the
// same thing on both ABIs even though nothing is patched here.
constexpr size_t kPatchSize = 20;

bool patch_entry(void *, void *) {
    LOGE("правка входов libc реализована только для arm64");
    return false;
}

void *tail_call_target(const void *, unsigned) { return nullptr; }

#endif

// ------------------------------------------------------------ branch protection
//
// bionic 17 is built with -mbranch-protection=standard, so its entries carry
// bti c / paciasp, and a loader sets PROT_BTI on an image the moment it declares
// GNU_PROPERTY_AARCH64_FEATURE_1_BTI. No Android image declares it — 11, 12, 12L,
// 13, 14, 15, 16 and 17, 64- and 32-bit, vold, libdl, all have no .note.gnu.property
// at all — which is why those instructions are inert hints today. But the direction is
// plain (bti c in libc: none at all on 11, 46 on 12/12L, 68 on 13, 69 on 14, 76 on 15,
// 64 on 16, 799 on 17; paciasp 0, 44, 44, 47, 48, 48, 47, 1443), so the flip is a
// matter of time, and the consequences land on the patch: offset 0 of a patched root
// becomes a guarded entry. That is what the bti jc in the patch is for, and this is
// where the assumption is written down and reported.
//
// Nothing here refuses to patch. The patch opens with a pad, so a guarded libc
// entry stays a legal target; the handler it branches to lives in our module,
// and OUR pages are guarded only if our module declares BTI too. It does not, and
// that is deliberate: a guarded handler would have to be a pad for the jump case
// as well, and the compiler emits paciasp at framed entries, which accepts calls
// only. Not declaring BTI keeps the branch into the handler unchecked.
//
// If the module's note and its build flags ever disagree, the handlers are pads
// by accident rather than by construction, and that is the one combination worth
// refusing — reported as -1 by hooks_bti_report.

#if defined(__ARM_FEATURE_BTI_DEFAULT) && __ARM_FEATURE_BTI_DEFAULT
constexpr int kBuildBti = 1;
#else
constexpr int kBuildBti = 0;
#endif

constexpr uint32_t kFeatBti = UNFUSE_FEAT_BTI;
constexpr uint32_t kFeatPac = UNFUSE_FEAT_PAC;
constexpr uint32_t kFeatGcs = UNFUSE_FEAT_GCS;

using ImageProps = UnfuseImageProps;

struct PropsQuery {
    const char *name_part;  // pick the image whose dlpi_name contains this...
    const void *addr;       // ...or, when null, the one containing this address
    ImageProps props;
};

// The note itself is parsed by gnu_props.h, which the host self-test exercises on
// synthetic notes; this callback only decides which image to look at and hands
// over one PT_GNU_PROPERTY segment at a time.
int props_cb(struct dl_phdr_info *info, size_t, void *data) {
    auto *q = static_cast<PropsQuery *>(data);

    int hit = 0;
    if (q->name_part != nullptr) {
        hit = info->dlpi_name != nullptr && strstr(info->dlpi_name, q->name_part) != nullptr;
    } else if (q->addr != nullptr) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(q->addr);
        for (int i = 0; i < info->dlpi_phnum && !hit; i++) {
            const ElfW(Phdr) &ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_LOAD) continue;
            const uintptr_t lo = info->dlpi_addr + ph.p_vaddr;
            hit = a >= lo && a < lo + ph.p_memsz;
        }
    }
    if (!hit) return 0;

    q->props.matched = 1;

    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) &ph = info->dlpi_phdr[i];
        if (ph.p_type != UNFUSE_PT_GNU_PROPERTY || ph.p_memsz == 0) continue;
        unfuse_props_parse(reinterpret_cast<const void *>(info->dlpi_addr + ph.p_vaddr),
                           ph.p_memsz, &q->props);
        break;
    }
    return 1;
}

ImageProps image_props(const char *name_part, const void *addr) {
    PropsQuery q{};
    q.name_part = name_part;
    q.addr = addr;
    dl_iterate_phdr(props_cb, &q);
    return q.props;
}

void props_str(const ImageProps &p, char *out, size_t len) {
    if (!p.matched) {
        snprintf(out, len, "образ не найден");
        return;
    }
    if (!p.has_note) {
        snprintf(out, len, "свойства GNU нет");
        return;
    }
    char bits[64];
    size_t u = 0;
    bits[0] = '\0';
    if (p.features & kFeatBti) u += snprintf(bits + u, sizeof bits - u, "BTI");
    if (p.features & kFeatPac) u += snprintf(bits + u, sizeof bits - u, "%sPAC", u ? "+" : "");
    if (p.features & kFeatGcs) u += snprintf(bits + u, sizeof bits - u, "%sGCS", u ? "+" : "");
    // A note with no AArch64 feature word is the normal case off-device (an x86
    // host has the note for its own ISA), so it reads as a fact, not a problem.
    snprintf(out, len, "%s", u ? bits : "свойство есть, AArch64-возможностей нет");
}

enum class State { Failed, Ok, Alias, Missing, Thunk, Small, Unknown };

struct HookDef {
    const char *name;
    void *handler;
};

// Roots only. A tail branch onto a root needs no entry here: the engine would
// resolve it, classify it and skip it anyway — by the size gate if it is shorter
// than the patch, by tail_call_target otherwise — because its calls already reach
// the root through .plt. Listing a name that is never patched buys nothing and
// hides the real coverage behind a label, so that coverage is checked where it can
// be measured instead: hookselftest.cpp on the device, verify-hook-targets.py
// statically. renameat is the one entry that is a root on some releases and a bare
// tail branch onto renameat2 on the others, and it is listed for the four where it
// is the root (header).
const HookDef kHooks[] = {
    {"open", reinterpret_cast<void *>(h_open)},
    {"open64", reinterpret_cast<void *>(h_open)},
    {"openat", reinterpret_cast<void *>(h_openat)},
    {"openat64", reinterpret_cast<void *>(h_openat)},
    {"__open_2", reinterpret_cast<void *>(h_open_2)},
    {"__openat_2", reinterpret_cast<void *>(h_openat_2)},
    {"mkdirat", reinterpret_cast<void *>(h_mkdirat)},
    {"fchmod", reinterpret_cast<void *>(h_fchmod)},
    {"fchmodat", reinterpret_cast<void *>(h_fchmodat)},
    {"renameat", reinterpret_cast<void *>(h_renameat)},
    {"renameat2", reinterpret_cast<void *>(h_renameat2)},
    {"linkat", reinterpret_cast<void *>(h_linkat)},
};

constexpr int kHookCount = static_cast<int>(sizeof(kHooks) / sizeof(kHooks[0]));

State g_state[kHookCount];
bool g_installed = false;

// The release the patch ran on (android_ver.h): filled by hooks_install, read by
// hooks_release. Only the .so build sees the table; this TU keeps the result.
UnfusePick g_ver;
bool g_ver_ready = false;

// open64 is the same address as open on 64-bit: alias, not re-patch.
void *g_patched[kHookCount];
int g_patched_n = 0;

bool already_patched(void *fn) {
    for (int i = 0; i < g_patched_n; i++) {
        if (g_patched[i] == fn) return true;
    }
    return false;
}

}  // namespace

int hooks_install(int *total) {
    if (total != nullptr) *total = kHookCount;

    // The release decides only what the tally is COMPARED against, not how the
    // entries are patched — that stays table-driven, see android_ver.h.
    g_ver = unfuse_pick(unfuse_sdk());
    g_ver_ready = true;

    if (g_installed) {
        int n = 0;
        for (int i = 0; i < kHookCount; i++) {
            if (g_state[i] == State::Ok || g_state[i] == State::Alias) n++;
        }
        return n;
    }
    g_installed = true;

    // Step 1: addresses (global scope, i.e. what app calls reach).
    void *fns[kHookCount];
    for (int i = 0; i < kHookCount; i++) fns[i] = dlsym(RTLD_DEFAULT, kHooks[i].name);

    // Step 2: sizes — 20 bytes into an 8/12-byte function clobbers the next.
    unsigned sizes[kHookCount];
    func_sizes(fns, kHookCount, sizes);

    // Step 3: patch only >= patch size and not a thunk. Both gates are live and the
    // order matters: a tail branch shorter than the patch never reaches the thunk
    // test (renameat from 14 on is 8 bytes, so it reads "коротка"), while one at or
    // above the gate is caught by tail_call_target (chmod 20, rename and link 28).
    // Neither is patched, and either way its calls already reach the root via .plt.
    // Only the ok count is returned; the other outcomes live in g_state, which
    // hooks_report() prints.
    int ok = 0;
    for (int i = 0; i < kHookCount; i++) {
        void *fn = fns[i];
        if (fn == nullptr) {
            g_state[i] = State::Missing;
            continue;
        }
        if (already_patched(fn)) {
            g_state[i] = State::Alias;
            ok++;
            continue;
        }
        if (sizes[i] == 0) {
            g_state[i] = State::Unknown;
            continue;
        }
        if (sizes[i] < kPatchSize) {
            g_state[i] = State::Small;
            continue;
        }
        if (tail_call_target(fn, sizes[i]) != nullptr) {
            g_state[i] = State::Thunk;
            continue;
        }
        if (patch_entry(fn, kHooks[i].handler)) {
            g_state[i] = State::Ok;
            if (g_patched_n < kHookCount) g_patched[g_patched_n++] = fn;
            ok++;
        } else {
            g_state[i] = State::Failed;
        }
    }

    // No log here: the caller prints the same tally once per boot. This runs on
    // every app launch, so a line here would flood the tag.
    return ok;
}

void hooks_report(char *buf, size_t len) {
    if (buf == nullptr || len == 0) return;
    size_t used = 0;
    buf[0] = '\0';

    for (int i = 0; i < kHookCount; i++) {
        const char *s = "?";
        switch (g_state[i]) {
            case State::Ok: s = "ok"; break;
            case State::Alias: s = "alias"; break;
            case State::Missing: s = "нет"; break;
            case State::Failed: s = "СБОЙ"; break;
            case State::Thunk: s = "переходник"; break;
            case State::Small: s = "коротка"; break;
            case State::Unknown: s = "размер?"; break;
        }
        const int w = snprintf(buf + used, len - used, "%s%s=%s", used ? " " : "", kHooks[i].name, s);
        if (w < 0 || static_cast<size_t>(w) >= len - used) break;
        used += static_cast<size_t>(w);
    }
}

int hooks_release(char *buf, size_t len) {
    if (buf != nullptr && len > 0) buf[0] = '\0';
    if (!g_ver_ready) return -1;

    unfuse_ver_str(&g_ver, buf, len);

    // -1 for a borrowed profile: its number was never measured, so a different
    // tally there is not a regression, only an unvalidated release.
    return g_ver.known ? g_ver.v->installed : -1;
}

int hooks_bti_report(char *buf, size_t len) {
    if (buf != nullptr && len > 0) buf[0] = '\0';

    // libc by name — it is what gets patched. The module by address, since its
    // path depends on where the manager installed it.
    const ImageProps libc = image_props("libc.so", nullptr);
    const ImageProps self = image_props(nullptr, reinterpret_cast<const void *>(&hooks_bti_report));

    if (buf != nullptr && len > 0) {
        char a[96], b[96];
        props_str(libc, a, sizeof a);
        props_str(self, b, sizeof b);
        snprintf(buf, len, "libc: %s; модуль: %s", a, b);
    }

    // The module's note and the flags it was built with have to agree: if the
    // note says BTI but the handlers were not compiled as landing pads, our pages
    // are guarded and the branch into them is unchecked for nothing.
    if (self.has_note && (self.features & kFeatBti) && !kBuildBti) return -1;

    return (libc.has_note && (libc.features & kFeatBti)) ? 1 : 0;
}

int hooks_path_is_storage(const char *path) {
    return (path != nullptr && path[0] == '/' && absolute_is_storage(path)) ? 1 : 0;
}
