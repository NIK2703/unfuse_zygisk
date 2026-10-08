/*
 * hook_libc.cpp — emulate sdcardfs mode handling in userspace.
 *
 * Raw /data/media access needs a POSIX ACL named entry for gid 9997
 * (AID_EVERYBODY). ACLs are not virtualisation: the kernel applies them to the
 * requested mode, so a 0600 create zeroes the mask (posix_acl_create_masq)
 * and the entry dies -> EACCES. sdcardfs synthesises 0770 dirs / 0660 files,
 * gid 9997, mask 0007, so the mode is shaped pre-syscall and the ACL lands
 * after the chmod (rename applies no default ACL).
 *
 * Only roots are patched (kHooks), see below; no trampoline (handlers syscall
 * directly), handlers must be reentrant (syscalls only). arm64 only.
 *
 * The patch is 20 bytes with a leading bti jc — see patch_entry; -1 from
 * hooks_bti_report when libc declares BTI but the module was built without it.
 *
 * A patched entry can be taken away by another in-process patcher (the GCam port
 * does exactly that). Instead of fighting it with a guard thread (forbidden in
 * this project), the open family is redirected through the bare __openat syscall
 * stub, found by form below — the port patches only open/openat, never the stub.
 *
 * The root COUNT is measured, not assumed: hookselftest.cpp on the device and
 * tools/verify-hook-targets.py device/libc/libc-arm64.so statically;
 * android_ver.h names the releases and hooks_release() reports any that comes
 * out otherwise.
 */

#include "hook_libc.h"

#include "android_ver.h"
#include "gnu_props.h"

#include "func_size.h"
#include "openat_stub.h"

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

namespace {

// AID_EVERYBODY: shared group of all apps in a profile (sdcardfs mounts).
constexpr uint32_t kAidEverybody = 9997;

// A thunk is never longer. arm64 only (see tail_call_target).
[[maybe_unused]] constexpr unsigned kThunkMax = 32;

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

// Shared storage: /storage/ (emulated/self/removable), /mnt/user,
// /mnt/runtime (same tree pre-Zygote bind), /mnt/pass_through (AOSP FUSE),
// /data/media (raw).
constexpr const char *kStoragePrefixes[] = {
    "/storage/",
    "/mnt/user/",
    "/mnt/runtime/",
    "/mnt/pass_through/",
    "/data/media/",
};

// /sdcard -> /storage/self/primary; whole component, so "/sdcardfoo" fails.
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

    // Relative to cwd; native chdir() exists, so never guess wrong.
    char cwd[512];
    const long n = syscall(SYS_getcwd, cwd, sizeof cwd);
    if (n <= 0) return false;
    return absolute_is_storage(cwd);
}

// sdcardfs view: owner kept, group rw (dirs +x), other cleared (mask 0007).
mode_t as_sdcardfs_file(mode_t m) {
    return (m & S_IRWXU) | (m & S_IRWXG) | S_IRGRP | S_IWGRP;
}

mode_t as_sdcardfs_dir(mode_t m) {
    return (m & S_IRWXU) | (m & S_IRWXG) | S_IRGRP | S_IWGRP | S_IXGRP;
}

mode_t widen_existing(int dirfd, const char *path, mode_t mode, int at_flags) {
    struct stat st;
    if (fstatat(dirfd, path, &st, at_flags) == 0 && S_ISDIR(st.st_mode)) {
        return as_sdcardfs_dir(mode);
    }
    return as_sdcardfs_file(mode);
}

// Five entries as vold::SetDefaultAcl (vold-16/Utils.cpp:142) /
// tools/storage-fix.c. The named 9997 entry and the mask both take the group
// perms (else the mask revokes access); OTHER cleared, as sdcardfs mask 0007.
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

// Mode first (it defines the ACL mask), then ACL: chmod rewrites the mode, so
// the ACL must land last. Preferred over the path variant (no cwd/path race).
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

bool exists_at(int dirfd, const char *path) {
    struct stat st;
    return fstatat(dirfd, path, &st, 0) == 0;
}

// Handlers replace the original and syscall directly: order is irrelevant.

bool needs_mode(int flags) {
    return (flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE;
}

// The shaped mode is what keeps the grant alive: a 0600 create cuts the ACL
// MASK to 0 (posix_acl_create_masq), so the inherited 9997 entry stops
// granting (touch -> 0660 with GROUP 9997 rw-; chmod 600 -> MASK ---). The ACL
// write below duplicates the parent's default ACL and stays because rename
// applies none (fix_after_move) and because vold re-runs SetDefaultAcl
// (PrepareAndroidDirs, EmulatedVolume.cpp:436; fscrypt_prepare_user_storage,
// FsCrypt.cpp:1027) after the module's pass on every boot — which only
// tools/vold-noacl.c (setxattr -> no-op) lets the 9997 default survive.
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
// rename applies no default ACL, so a 0600 file would land with none.
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

// The mkstemp family (mkstemp, mkostemp, mkstemps, mkostemps) is NOT hooked: it
// creates 0600, but bionic reaches its open through open@plt, so the patched
// open widens the mode and writes the ACL; hookselftest.cpp checks that chain.

#if defined(__aarch64__)

// 20 bytes: bti jc; ldr x17,#8; br x17; .quad <handler>. The literal is
// PC-relative (load at offset 4 reads offset 12) and the branch indirect, so no
// relocation is needed — but 20 bytes of room are, hence the size gate in
// hooks_install.
//
// bti jc is not decoration: once the image declares the BTI property the loader
// sets PROT_BTI, offset 0 becomes a guarded entry an indirect call must land
// on, and the pad has to come first or the patch faults. jc rather than c: a
// jump that did not come through x16/x17 arrives as BTYPE=jump, which bti c
// rejects.
//
// Both silent if broken: paciasp, the other legal pad (and what bionic uses at
// framed entries), signs x30 against the caller's SP, so the handler's ret
// returns to a signed address; and the branch must stay in x17 — the
// architecture exempts x16/x17 exactly so the PLT idiom may land on bti c.
constexpr uint32_t kBitJc = 0xd50324dfu;   // bti jc (hint #38)
constexpr uint32_t kLdrX17 = 0x58000051u;  // ldr x17, #8
constexpr uint32_t kBrX17 = 0xd61f0220u;   // br  x17
constexpr size_t kPatchSize = 20;

bool patch_entry(void *target, void *handler) {
    const uintptr_t addr = reinterpret_cast<uintptr_t>(target);
    if ((addr & 3u) != 0) {
        return false;
    }

    const long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) return false;
    const uintptr_t page = addr & ~(static_cast<uintptr_t>(ps) - 1);

    // Patch may straddle a page boundary; map both pages.
    const size_t span = static_cast<size_t>(addr - page) + kPatchSize;
    const size_t mlen = (span + static_cast<size_t>(ps) - 1) & ~(static_cast<size_t>(ps) - 1);

    // PROT_EXEC kept: .text is already VM_EXEC, not execmem (SELinux).
    if (mprotect(reinterpret_cast<void *>(page), mlen,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }

    // Not atomic, and no order of these stores is safe in general — each only
    // picks the least bad window: after the literal the original runs (correct
    // for every target); after the load, framed roots still run their own body
    // (x17 is call-clobbered) while the leaf syscall wrappers skip their
    // syscall and return a wrong value without crashing; after the branch the
    // wrappers work but a framed root runs through a live paciasp, which signs
    // x30 and breaks the handler's ret; after the pad, final state.
    //
    // Pad last: that window is one store wide, covers only framed roots, and
    // this runs from postAppSpecialize with one thread and nothing executing
    // the entry. volatile keeps the compiler from reordering the stores.
    auto *words = reinterpret_cast<volatile uint32_t *>(addr);
    auto *literal = reinterpret_cast<volatile uint64_t *>(addr + 12);
    *literal = reinterpret_cast<uint64_t>(handler);
    words[1] = kLdrX17;
    words[2] = kBrX17;
    words[0] = kBitJc;
    __builtin___clear_cache(reinterpret_cast<char *>(addr),
                            reinterpret_cast<char *>(addr) + kPatchSize);

    // Read back what was written: the pad is the word whose absence would be
    // silent today and fatal once a release declares BTI.
    const uint32_t got_pad = words[0];
    const uint32_t got_ldr = words[1];
    const uint32_t got_br = words[2];
    if (got_pad != kBitJc || got_ldr != kLdrX17 || got_br != kBrX17) {
        mprotect(reinterpret_cast<void *>(page), mlen, PROT_READ | PROT_EXEC);
        return false;
    }

    mprotect(reinterpret_cast<void *>(page), mlen, PROT_READ | PROT_EXEC);
    return true;
}

// Thunk = short body of arg shuffles + one tail branch -> that branch target,
// else nullptr. Short AND ends in an unconditional branch; bionic thunks
// shuffle args first and branch last, so the first instruction proves nothing.
//
// A 12-byte thunk vs a 20-byte patch would clobber the next function, so it is
// not patched: the branch goes via .plt, and bionic libc lacks -Bsymbolic, so
// intra-library calls use the GOT pointing at the already-patched entry
// (libc-16 R_AARCH64_JUMP_SLOT: creat/creat64 -> open@plt -> open; renameat ->
// renameat2@plt -> renameat2; mkstemps/mkostemps -> mktemp_internal ->
// open@plt). Safety is size: mkdir = 20 on 17 may be patched but need not be;
// verify-hook-targets.py uses that rule.
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

// Same width as the arm64 patch, so the size gate means the same on both ABIs.
constexpr size_t kPatchSize = 20;

bool patch_entry(void *, void *) {
    return false;
}

void *tail_call_target(const void *, unsigned) { return nullptr; }

#endif

// No Android image declares GNU_PROPERTY_AARCH64_FEATURE_1_BTI: 11, 12, 12L,
// 13, 14, 15, 16, 17, both ABIs, vold, libdl carry no .note.gnu.property at
// all, so a bionic 17's bti c / paciasp (-mbranch-protection=standard) are
// inert today — but bti c in libc goes none on 11, 46 on 12/12L, 68 on 13,
// 69 on 14, 76 on 15, 64 on 16, 799 on 17 and paciasp 0, 44, 44, 47, 48, 48,
// 47, 1443, so offset 0 of a patched root will become a guarded entry: that is
// what the bti jc in patch_entry is for.
//
// Nothing here refuses to patch: the pad keeps a guarded libc entry a legal
// target, and the handler stays unguarded because the module deliberately
// declares no BTI — a guarded handler would have to be a pad for the jump
// case too, while the compiler emits paciasp (calls only) at framed entries.
// Note and build flags disagreeing leaves the handlers pads by accident;
// hooks_bti_report answers -1.

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

// gnu_props.h parses the note (its host self-test exercises that); this
// callback only picks the image and hands over one PT_GNU_PROPERTY segment.
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

// Roots only; a tail branch onto a root needs no entry — the engine skips it
// (size gate, or tail_call_target) since its calls reach the root through .plt.
// Measured instead by hookselftest.cpp and verify-hook-targets.py. renameat is
// a root on 11/12/12L/13 and a tail branch onto renameat2 elsewhere.
//
// open/open64/openat/openat64 сняты с модуля: их патчит порт GCam, и любой
// чужой переходник на входе он считает хуком и роняет процесс (SIGBUS, см.
// «Стаб сисколла openat» ниже). Их работу берёт стаб сисколла __openat —
// отдельная цель, найденная по форме, которую порт не трогает. __open_2 и
// __openat_2 ОСТАЮТСЯ: они подставляют 0666 вместо нуля (снять их — обнулить
// ACL-маску), поэтому патчатся напрямую.
const HookDef kHooks[] = {
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

// Отдельная цель — стаб сисколла __openat (найден по форме, не из kHooks).
State g_stub_state = State::Missing;

bool g_installed = false;

// Last patch's release (android_ver.h): set by hooks_install, read by
// hooks_release.
UnfusePick g_ver;
bool g_ver_ready = false;

// +1: стаб __openat — тоже пропатченная цель, но не из kHooks.
void *g_patched[kHookCount + 1];
int g_patched_n = 0;

// --- Стаб сисколла openat: входы open/openat сняты --------------------------
//
// Почему сняты. Порт GCam (com.android.MGC_9_7_047) достаёт из ассетов
// codec_*.lck и dlopen-ит их; внутри лежит собственный патчер кода, который тоже
// закрывает входы open и openat. Root ему не нужен — правится своё адресное
// пространство, как и нам под uid приложения.
//
// Найдя на входе чужой переходник, он строит трамплин, который ЧИТАЕТ указатель
// по entry+12, — а перед этим сам затирает первые 16 байт. По entry+12 остаётся
// его слово плюс старшая половина нашего литерала (0x0000002d от базы
// 0x2d82815000): выходит 0x2d00000075, выравнивания нет, br x17 ловит
// BUS_ADRALN. На устройстве: его патч на ~432 мс от запуска, падение ещё через
// ~35 мс.
//
// Формой это не лечится: подстановка в живой процесс любой формы (порта,
// модуля, bti jc заменён на nop, литерал сдвинут на +16) роняет его одинаково —
// порт считает чужим хуком ЛЮБОЙ переходник на входе. Сторожевой поток тоже не
// ответ: сторожа в этом проекте запрещены (memory/MEMORY.md, «Запреты»).
//
// Остаётся убрать свой переходник с тех входов, которые порт патчит. open и
// openat снимаются, а их работу берёт единственный стаб сисколла __openat: всё
// open-семейство сходится в него — open/open64, openat/openat64, __open_2,
// __openat_2, все четыре адреса делают bl __openat, — а порт его не трогает.
// Разбор формы и доказательства: src/openat_stub.h.
//
// __open_2 и __openat_2 при этом ОСТАЮТСЯ входовыми целями: они зовут __openat
// с режимом 0, а модуль подставляет туда 0666 — нулевой режим обнулил бы
// ACL-маску. Сняв их, мы потеряли бы это.
#if defined(__aarch64__)

struct StubQuery {
    const char *name_part;  // "libc.so"
    void *addr;             // найденный стаб
    unsigned size;
};

int stub_cb(dl_phdr_info *info, size_t, void *data) {
    auto *q = static_cast<StubQuery *>(data);
    if (q->name_part == nullptr || info->dlpi_name == nullptr) return 0;
    if (strstr(info->dlpi_name, q->name_part) == nullptr) return 0;

    // Секций в рантайме нет — есть только сегменты, поэтому сканируем
    // исполняемый PT_LOAD целиком. Хостовый тест делает ровно то же самое
    // (tools/test-openat-stub.sh), иначе он проверял бы не тот вход.
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) &ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0) continue;

        const uintptr_t base = info->dlpi_addr + ph.p_vaddr;
        unsigned size = 0;
        const long off =
            openat_stub_find(reinterpret_cast<const void *>(base), ph.p_memsz, &size);
        if (off < 0) continue;

        q->addr = reinterpret_cast<void *>(base + static_cast<uintptr_t>(off));
        q->size = size;
        return 1;
    }
    return 0;
}

// Адрес стаба и его длина. nullptr — не нашли или нашли не один раз; тогда
// цель честно отчитается как отсутствующая, а не пропатчится наугад.
void *openat_stub_addr(unsigned *out_size) {
    StubQuery q{};
    q.name_part = "libc.so";
    dl_iterate_phdr(stub_cb, &q);
    if (out_size != nullptr) *out_size = q.size;
    return q.addr;
}

#else

// На v7a переходников нет вовсе: patch_entry под #else возвращает false.
void *openat_stub_addr(unsigned *out_size) {
    if (out_size != nullptr) *out_size = 0;
    return nullptr;
}

#endif

bool already_patched(void *fn) {
    for (int i = 0; i < g_patched_n; i++) {
        if (g_patched[i] == fn) return true;
    }
    return false;
}

}  // namespace

int hooks_install(int *total) {
    // kHooks + стаб __openat (отдельная цель по форме).
    if (total != nullptr) *total = kHookCount + 1;

    // The release picks what the tally is compared against, not how entries
    // are patched (android_ver.h).
    g_ver = unfuse_pick(unfuse_sdk());
    g_ver_ready = true;

    if (g_installed) {
        int n = 0;
        for (int i = 0; i < kHookCount; i++) {
            if (g_state[i] == State::Ok || g_state[i] == State::Alias) n++;
        }
        if (g_stub_state == State::Ok || g_stub_state == State::Alias) n++;
        return n;
    }
    g_installed = true;

    // Addresses in the global scope, i.e. what app calls reach.
    void *fns[kHookCount];
    for (int i = 0; i < kHookCount; i++) fns[i] = dlsym(RTLD_DEFAULT, kHooks[i].name);

    // Sizes: 20 bytes into an 8/12-byte function clobbers the next.
    unsigned sizes[kHookCount];
    func_sizes(fns, kHookCount, sizes);

    // Patch only >= patch size and not a thunk; both gates are live and the
    // order matters: renameat from 14 on is 8 bytes so it reads "коротка",
    // while chmod 20 and rename/link 28 are caught by tail_call_target.
    // Neither is patched, and either way its calls reach the root via .plt.
    // Only ok is returned; the rest stays in g_state for hooks_report().
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
            if (g_patched_n < kHookCount + 1) g_patched[g_patched_n++] = fn;
            ok++;
        } else {
            g_state[i] = State::Failed;
        }
    }

    // Отдельная цель: стаб сисколла __openat, найденный по форме (он локален,
    // dlsym его не видит). Он заменяет снятые входы open/open64/openat/openat64
    // — порт GCam патчит только open/openat, а стаб не трогает, поэтому конфликта
    // нет, и никакой сторож не нужен (memory/MEMORY.md, «Запреты»).
    {
        unsigned stub_size = 0;
        void *stub = openat_stub_addr(&stub_size);
        g_stub_state = State::Missing;
        if (stub != nullptr && stub_size >= kPatchSize && !already_patched(stub) &&
            patch_entry(stub, reinterpret_cast<void *>(h_openat))) {
            g_stub_state = State::Ok;
            if (g_patched_n < kHookCount + 1) g_patched[g_patched_n++] = stub;
            ok++;
        } else if (stub == nullptr) {
            g_stub_state = State::Missing;
        } else if (stub_size < kPatchSize) {
            g_stub_state = State::Small;
        } else if (already_patched(stub)) {
            g_stub_state = State::Alias;
        } else {
            g_stub_state = State::Failed;
        }
    }

// No log: the caller prints this tally once per boot, this runs per launch.
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

    // Степ сисколла openat — отдельная цель, не из kHooks (dlsym его не видит).
    {
        const char *s = "?";
        switch (g_stub_state) {
            case State::Ok: s = "ok"; break;
            case State::Alias: s = "alias"; break;
            case State::Missing: s = "нет"; break;
            case State::Failed: s = "СБОЙ"; break;
            case State::Thunk: s = "переходник"; break;
            case State::Small: s = "коротка"; break;
            case State::Unknown: s = "размер?"; break;
        }
        const int w = snprintf(buf + used, len - used, "%s%s=%s", used ? " " : "", "__openat", s);
        if (!(w < 0 || static_cast<size_t>(w) >= len - used)) used += static_cast<size_t>(w);
    }
}

int hooks_release(char *buf, size_t len) {
    if (buf != nullptr && len > 0) buf[0] = '\0';
    if (!g_ver_ready) return -1;

    unfuse_ver_str(&g_ver, buf, len);

    // -1 for a borrowed profile: never measured, so unvalidated, not a
// regression.
    return g_ver.known ? g_ver.v->installed : -1;
}

int hooks_bti_report(char *buf, size_t len) {
    if (buf != nullptr && len > 0) buf[0] = '\0';

    // libc by name (it gets patched), the module by address (path varies).
    const ImageProps libc = image_props("libc.so", nullptr);
    const ImageProps self = image_props(nullptr, reinterpret_cast<const void *>(&hooks_bti_report));

    if (buf != nullptr && len > 0) {
        char a[96], b[96];
        props_str(libc, a, sizeof a);
        props_str(self, b, sizeof b);
        snprintf(buf, len, "libc: %s; модуль: %s", a, b);
    }

    if (self.has_note && (self.features & kFeatBti) && !kBuildBti) return -1;

    return (libc.has_note && (libc.features & kFeatBti)) ? 1 : 0;
}

int hooks_path_is_storage(const char *path) {
    return (path != nullptr && path[0] == '/' && absolute_is_storage(path)) ? 1 : 0;
}
