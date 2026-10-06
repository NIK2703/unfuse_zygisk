/*
 * hook_libc.cpp — emulate sdcardfs mode handling in userspace.
 *
 * Raw /data/media access uses a POSIX ACL named entry for gid 9997
 * (AID_EVERYBODY). ACLs are not virtualisation: the kernel applies them to the
 * requested mode, so 0600 create/chmod zeroes the mask (posix_acl_create_masq)
 * and the entry dies -> EACCES. sdcardfs instead synthesises 0770 dirs / 0660
 * files, gid 9997 (mask=0007). Patch bionic entry points to shape the mode
 * pre-syscall: open/creat group rw + other cleared; mkdir also group x; chmod
 * never narrowed; rename/link into storage adds ACL (rename skips the default
 * ACL); mkstemp 0600 -> 0660. Only roots are patched; 8-16 byte thunks are
 * skipped (16 bytes would clobber the next function; they reach the root via
 * .plt). Entry patching covers loaded/later-dlopen'd/dlsym'd calls and needs no
 * trampoline (handlers syscall directly); handlers must be reentrant (syscalls
 * only). arm64 only.
 *
 *   tools/verify-hook-targets.py device/libc/libc-arm64.so
 */

#include "hook_libc.h"

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

extern "C" int h_creat(const char *path, mode_t mode) {
    return open_and_fix(AT_FDCWD, path, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

// Mode-less fortify variants. If O_CREAT still arrives (caller error; bionic
// aborts), use 0660, not 0, which would zero the ACL mask.
extern "C" int h_open_2(const char *path, int flags) {
    return open_and_fix(AT_FDCWD, path, flags, needs_mode(flags) ? 0666 : 0);
}

extern "C" int h_openat_2(int dirfd, const char *path, int flags) {
    return open_and_fix(dirfd, path, flags, needs_mode(flags) ? 0666 : 0);
}

extern "C" int h_mkdirat(int dirfd, const char *path, mode_t mode);

extern "C" int h_mkdir(const char *path, mode_t mode) {
    return h_mkdirat(AT_FDCWD, path, mode);
}

extern "C" int h_mkdirat(int dirfd, const char *path, mode_t mode) {
    const bool storage = is_storage(dirfd, path);
    if (storage) mode = as_sdcardfs_dir(mode);

    const int r = static_cast<int>(syscall(SYS_mkdirat, dirfd, path, mode));
    if (r == 0 && storage) fix_created_dir(dirfd, path);
    return r;
}

extern "C" int h_chmod(const char *path, mode_t mode) {
    if (is_storage(AT_FDCWD, path)) mode = widen_existing(AT_FDCWD, path, mode, 0);
    return static_cast<int>(syscall(SYS_fchmodat, AT_FDCWD, path, mode, 0));
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

extern "C" int h_rename(const char *oldp, const char *newp) {
    const int r = static_cast<int>(syscall(SYS_renameat, AT_FDCWD, oldp, AT_FDCWD, newp));
    if (r == 0) fix_after_move(AT_FDCWD, newp, AT_FDCWD, oldp);
    return r;
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

extern "C" int h_link(const char *oldp, const char *newp) {
    const int r = static_cast<int>(syscall(SYS_linkat, AT_FDCWD, oldp, AT_FDCWD, newp, 0));
    if (r == 0) fix_after_move(AT_FDCWD, newp, AT_FDCWD, oldp);
    return r;
}

extern "C" int h_linkat(int olddirfd, const char *oldp, int newdirfd, const char *newp, int flags) {
    const int r = static_cast<int>(syscall(SYS_linkat, olddirfd, oldp, newdirfd, newp, flags));
    if (r == 0) fix_after_move(newdirfd, newp, olddirfd, oldp);
    return r;
}

// mkstemp creates 0600 — exactly what zeroes the ACL mask. The original cannot
// be called (entry overwritten), so reimplement: fill six "X" randomly and
// open(O_CREAT|O_EXCL), retrying on EEXIST. Same contract.

constexpr char kLetters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

uint64_t rand64() {
    uint64_t v = 0;
    if (syscall(SYS_getrandom, &v, sizeof v, 0) == static_cast<long>(sizeof v)) return v;

    // getrandom unavailable — monotonic time + tid.
    struct {
        long sec;
        long nsec;
    } ts {};
    syscall(SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */, &ts);
    uint64_t f = static_cast<uint64_t>(ts.nsec) * 2654435761u;
    f ^= static_cast<uint64_t>(ts.sec) << 17;
    f ^= static_cast<uint64_t>(syscall(SYS_gettid)) * 40503u;
    return f;
}

int mkstemp_impl(char *tmpl, int suffixlen, int extra_flags, mode_t mode) {
    if (tmpl == nullptr) {
        errno = EINVAL;
        return -1;
    }
    const size_t len = strlen(tmpl);
    if (suffixlen < 0 || len < static_cast<size_t>(6 + suffixlen)) {
        errno = EINVAL;
        return -1;
    }

    char *x = tmpl + len - 6 - static_cast<size_t>(suffixlen);
    for (int i = 0; i < 6; i++) {
        if (x[i] != 'X') {
            errno = EINVAL;
            return -1;
        }
    }

    for (int attempt = 0; attempt < 128; attempt++) {
        const uint64_t r = rand64();
        for (int i = 0; i < 6; i++) x[i] = kLetters[(r >> (i * 6)) & 63];

        const int fd = static_cast<int>(
            syscall(SYS_openat, AT_FDCWD, tmpl, O_CREAT | O_EXCL | O_RDWR | extra_flags, mode));
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }

    errno = EEXIST;
    return -1;
}

// 0600 kept outside storage (private there); in storage it reads as 0660 anyway.
mode_t mkstemp_mode(const char *tmpl) {
    return is_storage(AT_FDCWD, tmpl) ? as_sdcardfs_file(0600) : 0600;
}

extern "C" int h_mkstemp(char *tmpl) { return mkstemp_impl(tmpl, 0, 0, mkstemp_mode(tmpl)); }

extern "C" int h_mkostemp(char *tmpl, int flags) {
    return mkstemp_impl(tmpl, 0, flags, mkstemp_mode(tmpl));
}

extern "C" int h_mkstemps(char *tmpl, int suffixlen) {
    return mkstemp_impl(tmpl, suffixlen, 0, mkstemp_mode(tmpl));
}

extern "C" int h_mkostemps(char *tmpl, int suffixlen, int flags) {
    return mkstemp_impl(tmpl, suffixlen, flags, mkstemp_mode(tmpl));
}

#if defined(__aarch64__)

// 16 bytes: ldr x17,#8; br x17; .quad <handler>. Literal PC-relative, branch
// register-indirect -> no range/instruction relocation; literal written first so
// the handler is in place before the branch appears.
constexpr uint32_t kLdrX17 = 0x58000051u;  // ldr x17, #8
constexpr uint32_t kBrX17 = 0xd61f0220u;   // br  x17
constexpr size_t kPatchSize = 16;

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

    memcpy(reinterpret_cast<void *>(addr + 8), &handler, sizeof handler);
    reinterpret_cast<uint32_t *>(addr)[1] = kBrX17;
    reinterpret_cast<uint32_t *>(addr)[0] = kLdrX17;
    __builtin___clear_cache(reinterpret_cast<char *>(addr),
                            reinterpret_cast<char *>(addr) + kPatchSize);

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
// A 12-byte thunk vs a 16-byte patch would clobber the next function, so no
// patch: the branch goes via .plt and bionic libc lacks -Bsymbolic, so
// intra-library calls use the GOT pointing at the already-patched entry
// (libc-16 R_AARCH64_JUMP_SLOT: creat/creat64 -> open@plt -> open; renameat ->
// renameat2@plt -> renameat2; mkstemps/mkostemps -> mktemp_internal -> open@plt).
//
// Safety is size: a 16-byte patch needs >=16 bytes, so a long thunk (mkdir = 16)
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

constexpr size_t kPatchSize = 16;

bool patch_entry(void *, void *) {
    LOGE("правка входов libc реализована только для arm64");
    return false;
}

void *tail_call_target(const void *, unsigned) { return nullptr; }

#endif

enum class State { Failed, Ok, Alias, Missing, Thunk, Small, Unknown };

struct HookDef {
    const char *name;
    void *handler;
};

// Only roots are patched; thunks (creat, mkdir, chmod, link, rename, renameat,
// mkstemp...) stay listed but are detected and skipped — their calls reach the
// root via .plt. Listing them patches a ROM where a thunk is the real impl and
// makes each name's fate explicit.
const HookDef kHooks[] = {
    {"open", reinterpret_cast<void *>(h_open)},
    {"open64", reinterpret_cast<void *>(h_open)},
    {"openat", reinterpret_cast<void *>(h_openat)},
    {"openat64", reinterpret_cast<void *>(h_openat)},
    {"creat", reinterpret_cast<void *>(h_creat)},
    {"creat64", reinterpret_cast<void *>(h_creat)},
    {"__open_2", reinterpret_cast<void *>(h_open_2)},
    {"__openat_2", reinterpret_cast<void *>(h_openat_2)},
    {"mkdir", reinterpret_cast<void *>(h_mkdir)},
    {"mkdirat", reinterpret_cast<void *>(h_mkdirat)},
    {"chmod", reinterpret_cast<void *>(h_chmod)},
    {"fchmod", reinterpret_cast<void *>(h_fchmod)},
    {"fchmodat", reinterpret_cast<void *>(h_fchmodat)},
    {"rename", reinterpret_cast<void *>(h_rename)},
    {"renameat", reinterpret_cast<void *>(h_renameat)},
    {"renameat2", reinterpret_cast<void *>(h_renameat2)},
    {"link", reinterpret_cast<void *>(h_link)},
    {"linkat", reinterpret_cast<void *>(h_linkat)},
    {"mkstemp", reinterpret_cast<void *>(h_mkstemp)},
    {"mkostemp", reinterpret_cast<void *>(h_mkostemp)},
    {"mkstemps", reinterpret_cast<void *>(h_mkstemps)},
    {"mkostemps", reinterpret_cast<void *>(h_mkostemps)},
};

constexpr int kHookCount = static_cast<int>(sizeof(kHooks) / sizeof(kHooks[0]));

State g_state[kHookCount];
bool g_installed = false;

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

    // Step 2: sizes — 16 bytes into an 8/12-byte function clobbers the next.
    unsigned sizes[kHookCount];
    func_sizes(fns, kHookCount, sizes);

    // Step 3: patch only >= patch size and not a thunk (a long thunk like mkdir
    // is patchable but pointless — its calls already reach the patched root).
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

int hooks_path_is_storage(const char *path) {
    return (path != nullptr && path[0] == '/' && absolute_is_storage(path)) ? 1 : 0;
}
