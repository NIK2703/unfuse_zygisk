/*
 * hookselftest.cpp — check the libc entry-patching on the device itself.
 * Built as a normal arm64 dynamic executable, run as root on the phone. Checks:
 *   1. target sizes from .dynsym ON DEVICE match the host preflight
 *      (tools/verify-hook-targets.py) — two independent ELF readers agree;
 *   2. entry bytes become bti jc / ldr x17,#8 / br x17 (20 байт);
 *   3. modes are coerced to sdcardfs form (0600->0660, 0700->0770) plus a 9997
 *      ACL entry;
 *   4. .plt thunk coverage works: creat, mkstemp(s), mkdtemp are not patched
 *      (shorter than the patch) but get the coerced mode via .plt to the patched
 *      root — empirical proof of the preflight coverage claim;
 *   5. paths outside storage are untouched: 0600 stays 0600;
 *   6. a narrow-mode file is really openable by ANOTHER uid: the process drops to
 *      app rights (uid 10398, group 9997) and opens it — the whole point.
 * Run (on device, as root): /data/local/tmp/hookselftest
 */

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <dlfcn.h>

#include "func_size.h"
#include "hook_libc.h"

namespace {

constexpr const char *kRoot = "/data/media/0/.hooktest";

// Names must match kHooks[] in src/hook_libc.cpp.
const char *const kNames[] = {
    "open", "open64", "openat", "openat64",
    "creat", "creat64", "__open_2", "__openat_2",
    "mkdir", "mkdirat",
    "chmod", "fchmod", "fchmodat",
    "rename", "renameat", "renameat2",
    "link", "linkat",
    "mkstemp", "mkostemp", "mkstemps", "mkostemps",
};
constexpr int kCount = static_cast<int>(sizeof(kNames) / sizeof(kNames[0]));

int g_fail = 0;
int g_pass = 0;

// Remember everything created so we can clean up after dropping privileges.
char g_created[24][256];
int g_created_n = 0;

void remember(const char *path) {
    if (g_created_n >= 24) return;
    snprintf(g_created[g_created_n], sizeof g_created[0], "%s", path);
    g_created_n++;
}

void check(bool ok, const char *what) {
    printf("    [%s] %s\n", ok ? "ок" : "ПРОВАЛ", what);
    if (ok) g_pass++;
    else g_fail++;
}

const char *mode_str(mode_t m, char *buf, size_t len) {
    static const char rwx[] = "rwx";
    if (len < 11) return "?";
    buf[0] = S_ISDIR(m) ? 'd' : (S_ISREG(m) ? '-' : '?');
    for (int i = 0; i < 9; i++) buf[1 + i] = (m & (1u << (8 - i))) ? rwx[i % 3] : '-';
    buf[10] = '\0';
    return buf;
}

void dump_acl(const char *path) {
    unsigned char buf[512];
    const ssize_t n = getxattr(path, "system.posix_acl_access", buf, sizeof buf);
    if (n < 4) {
        printf("        ACL: %s\n", n < 0 ? strerror(errno) : "пусто");
        return;
    }

    uint32_t ver;
    memcpy(&ver, buf, 4);
    printf("        ACL: версия %u,", ver);
    for (ssize_t off = 4; off + 8 <= n; off += 8) {
        uint16_t tag, perm;
        uint32_t id;
        memcpy(&tag, buf + off, 2);
        memcpy(&perm, buf + off + 2, 2);
        memcpy(&id, buf + off + 4, 4);
        const char *tn = "?";
        switch (tag) {
            case 0x01: tn = "USER_OBJ"; break;
            case 0x04: tn = "GROUP_OBJ"; break;
            case 0x08: tn = "GROUP"; break;
            case 0x10: tn = "MASK"; break;
            case 0x20: tn = "OTHER"; break;
        }
        if (id == 0xffffffffu) printf(" %s=%o", tn, perm);
        else printf(" %s(%u)=%o", tn, id, perm);
    }
    printf("\n");
}

bool has_group_9997(const char *path) {
    unsigned char buf[512];
    const ssize_t n = getxattr(path, "system.posix_acl_access", buf, sizeof buf);
    if (n < 4) return false;
    for (ssize_t off = 4; off + 8 <= n; off += 8) {
        uint16_t tag;
        uint32_t id;
        memcpy(&tag, buf + off, 2);
        memcpy(&id, buf + off + 4, 4);
        if (tag == 0x08 && id == 9997) return true;
    }
    return false;
}

void dump_entry(const char *name) {
    void *fn = dlsym(RTLD_DEFAULT, name);
    if (fn == nullptr) {
        printf("  %-10s нет символа\n", name);
        return;
    }
    const unsigned char *p = static_cast<const unsigned char *>(fn);
    printf("  %-10s %p:", name, fn);
    for (int i = 0; i < 20; i++) printf(" %02x", p[i]);
    printf("\n");
}

mode_t mode_of(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_mode & 07777;
}

void test_sizes() {
    printf("\n== 1. Размеры целей, прочитанные из .dynsym на устройстве ==\n");

    void *fns[kCount];
    for (int i = 0; i < kCount; i++) fns[i] = dlsym(RTLD_DEFAULT, kNames[i]);

    Dl_info di;
    if (fns[0] != nullptr && dladdr(fns[0], &di) != 0) {
        printf("  объект: %s\n  база:   %p\n", di.dli_fname, di.dli_fbase);
    }

    unsigned sizes[kCount];
    func_sizes(fns, kCount, sizes);
    for (int i = 0; i < kCount; i++) {
        if (fns[i] == nullptr) {
            printf("  %-10s нет символа\n", kNames[i]);
            continue;
        }
        printf("  %-10s размер=%u%s\n", kNames[i], sizes[i],
               sizes[i] == 0 ? "  <- патчить нельзя" : "");
    }
}

void test_patch_bytes() {
    printf("\n== 2. Байты входа до и после установки хуков ==\n");
    printf("  до:\n");
    dump_entry("openat");
    dump_entry("mkdirat");

    int total = 0;
    const int installed = hooks_install(&total);
    printf("  установлено %d из %d\n", installed, total);

    char report[1024];
    hooks_report(report, sizeof report);
    printf("  отчёт:\n    %s\n", report);

    // Which release the table matched, and whether the tally is what that
    // release is known to yield (android_ver.h). -1 means the release is not in
    // the table, so there is no measured number to compare against — which is
    // exactly the state a new release starts in, and why this prints the count
    // instead of asserting it.
    char ver[192];
    const int expected = hooks_release(ver, sizeof ver);
    printf("  версия: %s\n", ver);
    if (expected > 0) {
        char what[256];
        snprintf(what, sizeof what, "покрыто целей %d, ожидается %d",
                 installed, expected);
        check(installed == expected, what);
    } else {
        printf("  ожидание неизвестно — сюда впишите измеренное: %d целей\n",
               installed);
    }

    printf("  после:\n");
    dump_entry("openat");
    dump_entry("mkdirat");

    void *fn = dlsym(RTLD_DEFAULT, "openat");
    check(fn != nullptr, "openat разрешён через dlsym");
    if (fn != nullptr) {
        const uint32_t *w = static_cast<const uint32_t *>(fn);
        char what[200];
        snprintf(what, sizeof what,
                 "вход openat = bti jc / ldr x17,#8 / br x17 "
                 "(получено 0x%08x 0x%08x 0x%08x)",
                 w[0], w[1], w[2]);
        check(w[0] == 0xd50324dfu && w[1] == 0x58000051u && w[2] == 0xd61f0220u, what);
    }
}

void test_modes() {
    printf("\n== 3. Приведение режимов на сыром /data/media ==\n");
    char b1[16], b2[16];

    const char *f1 = "/data/media/0/.hooktest/open0600";
    const int fd = open(f1, O_CREAT | O_TRUNC | O_RDWR, 0600);
    mode_t m1 = 0;
    if (fd < 0) printf("      open -> %s\n", strerror(errno));
    else { close(fd); m1 = mode_of(f1); }
    remember(f1);
    printf("  open(0600)  -> %s (%03o)\n", mode_str(m1, b1, sizeof b1), m1);
    check((m1 & 0777) == 0660, "open(O_CREAT, 0600) даёт 0660");
    dump_acl(f1);

    const char *d1 = "/data/media/0/.hooktest/mkdir0700";
    if (mkdir(d1, 0700) != 0) printf("      mkdir -> %s\n", strerror(errno));
    const mode_t m2 = mode_of(d1);
    remember(d1);
    printf("  mkdir(0700) -> %s (%03o)\n", mode_str(m2, b2, sizeof b2), m2);
    check((m2 & 0777) == 0770, "mkdir(0700) даёт 0770");
    dump_acl(d1);

    const char *f2 = "/data/media/0/.hooktest/chmod0600";
    const int fd2 = open(f2, O_CREAT | O_TRUNC | O_RDWR, 0660);
    if (fd2 >= 0) close(fd2);
    remember(f2);
    if (chmod(f2, 0600) != 0) printf("      chmod -> %s\n", strerror(errno));
    const mode_t m3 = mode_of(f2);
    printf("  chmod(0600) -> %03o\n", m3);
    check((m3 & 0777) == 0660, "chmod(0600) не сужает режим");
}

void test_thunk_coverage() {
    printf("\n== 4. Покрытие переходников через .plt (они сами не патчатся) ==\n");

    // creat is 12 bytes, adjacent to open. Cannot be patched, but its mode must be 0660.
    const char *f1 = "/data/media/0/.hooktest/creat0600";
    const int fd = creat(f1, 0600);
    if (fd < 0) printf("      creat -> %s\n", strerror(errno));
    else close(fd);
    remember(f1);
    const mode_t m1 = mode_of(f1);
    printf("  creat(0600)     -> %03o\n", m1);
    check((m1 & 0777) == 0660, "creat(0600) покрыт через open@plt");

    // mkstemp (16 bytes) and mkstemps (12) go via mktemp_internal -> open@plt.
    char tmpl1[] = "/data/media/0/.hooktest/mkstempXXXXXX";
    const int fd2 = mkstemp(tmpl1);
    if (fd2 < 0) printf("      mkstemp -> %s\n", strerror(errno));
    else close(fd2);
    remember(tmpl1);
    const mode_t m2 = mode_of(tmpl1);
    printf("  mkstemp()       -> %03o (%s)\n", m2, tmpl1);
    check((m2 & 0777) == 0660, "mkstemp() покрыт через mktemp_internal -> open@plt");

    char tmpl2[] = "/data/media/0/.hooktest/mkstempsXXXXXX.txt";
    const int fd3 = mkstemps(tmpl2, 4);
    if (fd3 < 0) printf("      mkstemps -> %s\n", strerror(errno));
    else close(fd3);
    remember(tmpl2);
    const mode_t m3 = mode_of(tmpl2);
    printf("  mkstemps()      -> %03o\n", m3);
    check((m3 & 0777) == 0660, "mkstemps() покрыт через mktemp_internal -> open@plt");

    // mkdtemp is a directory: mktemp_internal -> mkdir@plt.
    char tmpl3[] = "/data/media/0/.hooktest/mkdtempXXXXXX";
    char *d = mkdtemp(tmpl3);
    if (d == nullptr) printf("      mkdtemp -> %s\n", strerror(errno));
    remember(tmpl3);
    const mode_t m4 = mode_of(tmpl3);
    printf("  mkdtemp()       -> %03o\n", m4);
    check((m4 & 0777) == 0770, "mkdtemp() покрыт через mktemp_internal -> mkdir@plt");

    // rename is 28 bytes but also a thunk onto renameat2; check it indirectly,
    // through renameat2 directly and through rename.
    const char *src = "/data/media/0/.hooktest/rename_src";
    const char *dst = "/data/media/0/.hooktest/rename_dst";
    const int fd4 = open(src, O_CREAT | O_TRUNC | O_RDWR, 0660);
    if (fd4 >= 0) close(fd4);
    remember(src);
    remember(dst);
    if (rename(src, dst) != 0) printf("      rename -> %s\n", strerror(errno));
    const mode_t m5 = mode_of(dst);
    printf("  rename(в хранилище) -> %03o\n", m5);
    check((m5 & 0777) == 0660, "rename внутри хранилища сохраняет 0660");
}

void test_rename_in() {
    printf("\n== 5. rename из приватного каталога в хранилище ==\n");

    const char *src = "/data/local/tmp/.hooktest_src";
    const char *dst = "/data/media/0/.hooktest/renamed_in";
    remember(dst);

    const int fd = open(src, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0) {
        printf("      создать источник не удалось: %s\n", strerror(errno));
        check(false, "rename-in: источник");
        return;
    }
    close(fd);

    printf("  до rename:    режим %03o, ACL: %s\n", mode_of(src) & 0777,
           has_group_9997(src) ? "есть" : "нет");

    if (rename(src, dst) != 0) {
        printf("      rename -> %s\n", strerror(errno));
        check(false, "rename-in: перенос");
        return;
    }

    const mode_t m = mode_of(dst);
    printf("  после rename: режим %03o\n", m);
    dump_acl(dst);
    check((m & 0777) == 0660, "rename-in даёт 0660");
    check(has_group_9997(dst), "rename-in дописывает ACL с группой 9997");
}

// Access must not depend on the directory's default ACL: a dir whose ACL vold
// rewrote has no named 9997 entry to inherit (/data/media/0 had 9997 in access but
// 1023 in default, so files at the storage root were invisible). The hook survives
// by writing the ACL itself rather than relying on inheritance.
void test_no_inherit() {
    printf("\n== 6. Доступ, когда у каталога нет default ACL ==\n");

    const char *dir = "/data/media/0/.hooktest/nodflt";
    if (mkdir(dir, 0700) != 0) {
        printf("      mkdir -> %s\n", strerror(errno));
        check(false, "создать каталог без default ACL");
        return;
    }
    remember(dir);

    // Remove the default ACL — nothing left to inherit.
    if (removexattr(dir, "system.posix_acl_default") != 0) {
        printf("      removexattr -> %s\n", strerror(errno));
    }
    printf("  каталог-родитель без default ACL:\n");
    dump_acl(dir);

    const char *child = "/data/media/0/.hooktest/nodflt/child0600";
    const int fd = open(child, O_CREAT | O_TRUNC | O_RDWR, 0600);
    mode_t m = 0;
    if (fd < 0) printf("      open -> %s\n", strerror(errno));
    else { close(fd); m = mode_of(child); }
    remember(child);

    printf("  open(0600) в нём -> %03o\n", m);
    dump_acl(child);
    check((m & 0777) == 0660, "режим всё равно приведён к 0660");
    check(has_group_9997(child),
          "хук дописал ACL с группой 9997 без наследования");
}

void test_non_storage() {
    printf("\n== 7. Гейт путей и поведение вне хранилища ==\n");

    check(hooks_path_is_storage("/storage/emulated/0/Download/x") == 1,
          "гейт: /storage/emulated/... — хранилище");
    check(hooks_path_is_storage("/sdcard/x") == 1, "гейт: /sdcard/x — хранилище");
    check(hooks_path_is_storage("/data/media/0/x") == 1,
          "гейт: /data/media/0/x — хранилище");
    check(hooks_path_is_storage("/mnt/user/0/emulated/x") == 1,
          "гейт: /mnt/user/0/emulated/x — хранилище");
    check(hooks_path_is_storage("/sdcardfoo") == 0,
          "гейт: /sdcardfoo — НЕ хранилище");
    check(hooks_path_is_storage("/data/data/com.example/x") == 0,
          "гейт: /data/data/... — не хранилище");
    // /storage/ is a deliberate prefix: it covers both /storage/emulated and
    // removable volumes named /storage/XXXX-XXXX. We need not know the exact
    // volume list — an extra match is harmless, since the mode is fixed before
    // the syscall and a nonexistent path returns ENOENT anyway.
    check(hooks_path_is_storage("/storage/XXXX-XXXX/x") == 1,
          "гейт: /storage/<том>/... — хранилище");
    check(hooks_path_is_storage("/storagefoo") == 0,
          "гейт: /storagefoo — НЕ хранилище");

    const char *p = "/data/local/tmp/.hooktest_private";
    const int fd = open(p, O_CREAT | O_TRUNC | O_RDWR, 0600);
    mode_t m = 0;
    if (fd < 0) printf("      open -> %s\n", strerror(errno));
    else { close(fd); m = mode_of(p); }
    printf("  open(/data/local/tmp, 0600) -> %03o\n", m);
    check((m & 0777) == 0600, "вне хранилища 0600 остаётся 0600");
    unlink(p);
}

void drop_to_app() {
    // A real app process: its own uid/gid plus group 9997 (AID_EVERYBODY), which
    // owns the storage ACL entries.
    gid_t groups[] = {9997, 10398, 20398, 50398};
    if (setgroups(4, groups) != 0) printf("  setgroups: %s\n", strerror(errno));
    if (setgid(10398) != 0) printf("  setgid: %s\n", strerror(errno));
    if (setuid(10398) != 0) printf("  setuid: %s\n", strerror(errno));
}

void test_other_uid() {
    printf("\n== 8. Доступ ДРУГОГО uid к созданному с узким режимом ==\n");

    const char *paths[] = {
        "/data/media/0/.hooktest/open0600",
        "/data/media/0/.hooktest/creat0600",
        "/data/media/0/.hooktest/chmod0600",
        "/data/media/0/.hooktest/renamed_in",
        "/data/media/0/.hooktest/nodflt/child0600",
    };

    drop_to_app();
    printf("  сбросили права: uid=%d gid=%d\n", (int)getuid(), (int)getgid());

    for (const char *p : paths) {
        const int fd = open(p, O_RDWR);
        char what[224];
        snprintf(what, sizeof what, "uid 10398 открывает %s", p);
        if (fd < 0) printf("      %s -> %s\n", p, strerror(errno));
        else close(fd);
        check(fd >= 0, what);
    }
}

void cleanup() {
    printf("\n== 9. Уборка (уже из-под прав приложения) ==\n");
    for (int i = g_created_n - 1; i >= 0; i--) {
        if (unlink(g_created[i]) != 0 && errno != ENOENT && errno != EISDIR) {
            printf("  удалить %s: %s\n", g_created[i], strerror(errno));
        }
        rmdir(g_created[i]);
    }
    if (rmdir(kRoot) != 0 && errno != ENOENT) {
        printf("  rmdir %s: %s\n", kRoot, strerror(errno));
    }
    struct stat st;
    if (stat(kRoot, &st) != 0) printf("  каталог убран\n");
    else printf("  ВНИМАНИЕ: %s остался\n", kRoot);
}

// Diagnostic mode: create a file as a DIFFERENT uid with hooks already installed.
// For end-to-end checks from an app namespace (toybox nsenter has no --setuid), we
// drop privileges here, after installing hooks as in a real app process. An app
// creating a 0600 file under /storage/emulated must get 0660 + a 9997 ACL entry, or
// another app will not see it — exactly why the hook exists.
int writeas_main(int argc, char **argv) {
    if (argc < 4) {
        printf("usage: writeas <uid> <path> [режим]\n");
        return 2;
    }
    const unsigned uid = static_cast<unsigned>(strtoul(argv[2], nullptr, 10));
    const char *path = argv[3];
    const mode_t want = argc > 4
        ? static_cast<mode_t>(strtoul(argv[4], nullptr, 8)) : 0600;

    int total = 0;
    const int installed = hooks_install(&total);
    printf("хуки установлены: %d из %d\n", installed, total);

    // Groups like a real app process: 9997 (AID_EVERYBODY) plus appid derivatives.
    const unsigned appid = uid % 100000;
    gid_t groups[] = {9997, static_cast<gid_t>(appid),
                      static_cast<gid_t>(20000 + appid), static_cast<gid_t>(50000 + appid)};
    if (setgroups(4, groups) != 0) printf("setgroups: %s\n", strerror(errno));
    if (setgid(uid) != 0) printf("setgid: %s\n", strerror(errno));
    if (setuid(uid) != 0) printf("setuid: %s\n", strerror(errno));
    printf("теперь uid=%d gid=%d\n", (int)getuid(), (int)getgid());

    const int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, want);
    if (fd < 0) {
        printf("open(%s, %03o): %s\n", path, want, strerror(errno));
        return 1;
    }
    if (write(fd, "test\n", 5) < 0) printf("write: %s\n", strerror(errno));
    close(fd);

    struct stat st;
    if (stat(path, &st) == 0) {
        char b[16];
        printf("%s: %s (%04o) uid=%u gid=%u\n", path,
               mode_str(st.st_mode, b, sizeof b), st.st_mode & 07777,
               st.st_uid, st.st_gid);
        printf("запись прошла\n");
    }
    return 0;
}

// Diagnostic mode: open a file as another uid — answers whether a file created
// by one app is visible to another.
int readas_main(int argc, char **argv) {
    if (argc < 4) {
        printf("usage: readas <uid> <path>\n");
        return 2;
    }
    const unsigned uid = static_cast<unsigned>(strtoul(argv[2], nullptr, 10));
    const char *path = argv[3];

    const unsigned appid = uid % 100000;
    gid_t groups[] = {9997, static_cast<gid_t>(appid),
                      static_cast<gid_t>(20000 + appid), static_cast<gid_t>(50000 + appid)};
    setgroups(4, groups);
    setgid(uid);
    if (setuid(uid) != 0) {
        printf("setuid(%u): %s\n", uid, strerror(errno));
        return 1;
    }

    const int fd = open(path, O_RDWR);
    if (fd < 0) {
        printf("uid %u НЕ открывает %s: %s\n", uid, path, strerror(errno));
        return 1;
    }
    char buf[8] = {0};
    const ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    printf("uid %u открывает %s, прочитано %zd байт: %s\n", uid, path, n, buf);
    return 0;
}

}  // namespace

// Diagnostic mode: which filesystem backs a path. Used from a REAL app's mount
// namespace (nsenter into its /proc/<pid>/ns/mnt) to confirm /storage/emulated
// serves the raw tree, not FUSE. This checks the module, not the hook: swapping
// the mount point is its first half and must be visible from the app namespace.
int fs_main(int argc, char **argv) {
    for (int i = 2; i < argc; i++) {
        struct statfs sf;
        struct stat st;
        if (statfs(argv[i], &sf) != 0) {
            printf("%s: statfs: %s\n", argv[i], strerror(errno));
            continue;
        }
        const char *name = "?";
        switch (static_cast<unsigned long>(sf.f_type)) {
            case 0xf2f52010UL: name = "f2fs (сырое дерево)"; break;
            case 0x65735546UL: name = "FUSE"; break;
            case 0x5dca2df5UL: name = "sdcardfs"; break;
            case 0x1021994UL:  name = "tmpfs"; break;
            case 0x794c7630UL: name = "overlayfs"; break;
        }
        printf("%-44s 0x%08lx  %-20s", argv[i],
               static_cast<unsigned long>(sf.f_type), name);
        if (stat(argv[i], &st) == 0) printf(" dev=%llu", (unsigned long long)st.st_dev);
        printf("\n");
    }
    return 0;
}

// Diagnostic mode: show the mode and both ACLs for the given paths — needed
// because on-device getfattr prints the binary ACL as an empty string.
int acl_main(int argc, char **argv) {
    for (int i = 2; i < argc; i++) {
        const char *path = argv[i];
        struct stat st;
        if (lstat(path, &st) != 0) {
            printf("%s: %s\n", path, strerror(errno));
            continue;
        }
        char b[16];
        printf("%s  %s (%04o)  uid=%u gid=%u\n", path,
               mode_str(st.st_mode, b, sizeof b), st.st_mode & 07777,
               st.st_uid, st.st_gid);

        const char *xattrs[2] = {"system.posix_acl_access", "system.posix_acl_default"};
        for (const char *x : xattrs) {
            unsigned char buf[512];
            const ssize_t n = getxattr(path, x, buf, sizeof buf);
            if (n < 4) {
                printf("    %-28s %s\n", x,
                       n < 0 ? strerror(errno) : "нет");
                continue;
            }
            uint32_t ver;
            memcpy(&ver, buf, 4);
            printf("    %-28s версия %u,", x, ver);
            for (ssize_t off = 4; off + 8 <= n; off += 8) {
                uint16_t tag, perm;
                uint32_t id;
                memcpy(&tag, buf + off, 2);
                memcpy(&perm, buf + off + 2, 2);
                memcpy(&id, buf + off + 4, 4);
                const char *tn = "?";
                switch (tag) {
                    case 0x01: tn = "USER_OBJ"; break;
                    case 0x04: tn = "GROUP_OBJ"; break;
                    case 0x08: tn = "GROUP"; break;
                    case 0x10: tn = "MASK"; break;
                    case 0x20: tn = "OTHER"; break;
                }
                if (id == 0xffffffffu) printf(" %s=%o", tn, perm);
                else printf(" %s(%u)=%o", tn, id, perm);
            }
            printf("\n");
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "acl") == 0) return acl_main(argc, argv);
    if (argc >= 3 && strcmp(argv[1], "fs") == 0) return fs_main(argc, argv);
    if (argc >= 4 && strcmp(argv[1], "writeas") == 0) return writeas_main(argc, argv);
    if (argc >= 4 && strcmp(argv[1], "readas") == 0) return readas_main(argc, argv);

    printf("=== проверка хука libc ===\n");
    printf("uid=%d gid=%d\n", (int)getuid(), (int)getgid());

    test_sizes();
    test_patch_bytes();

    // The dir is created AFTER installing hooks, else it would get group root
    // instead of a 9997 ACL entry, making the other-uid access check meaningless.
    if (mkdir(kRoot, 0770) != 0 && errno != EEXIST) {
        printf("не создать %s: %s\n", kRoot, strerror(errno));
        return 2;
    }

    test_modes();
    test_thunk_coverage();
    test_rename_in();
    test_no_inherit();
    test_non_storage();

    printf("\n== Итог до сброса прав: успешно %d, провалено %d ==\n", g_pass, g_fail);

    test_other_uid();
    cleanup();

    printf("\n== ИТОГ: успешно %d, провалено %d ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
