/*
 * storage-fix — permissions on the raw /data/media tree for kernels without sdcardfs.
 *
 * sdcardfs synthesises rights: from the mount's mask/gid it gives any process in
 * group 9997 (AID_EVERYBODY) 0770 dirs and 0660 files regardless of the lower FS.
 * Without it, /data/media is 1023:1023 (media_rw) with modes 0550/2770/0670, so apps
 * (which lack 1023) cannot even enter. We place a POSIX ACL with a named entry for
 * group 9997 — the same gid the sdcardfs read/write/full mounts use
 * (system-core-16/sdcard/sdcard.cpp:182-206); 9997 is shared by all apps
 * (android_filesystem_config.h:166).
 *
 * Why ACL, not chmod: a mode is system-wide; an ACL entry sits on top without
 * disturbing MediaProvider or vold.
 *   - vold resets owner+mode of /data/media and Android/ each boot
 *     (fs_prepare_dir → chown+chmod); chmod rewrites only USER_OBJ/GROUP_OBJ/MASK/
 *     OTHER and leaves named entries, so the grant survives.
 *   - app umask 0077 would give 0600, but a dir's default ACL makes the kernel skip
 *     umask (vfs_create() omits `mode &= ~current_umask()`; posix_acl_create()
 *     intersects with the ACL), so an app-created file is 0660 with the inherited
 *     9997 entry.
 * Dirs get access + default ACL: the first opens the dir, the second is inherited by
 * everything created inside (incl. by vold and MediaProvider).
 *
 * Modes: <dir>... grant group 9997 rwx/rw recursively; --traverse <dir> r-x on the
 * dir only (volume root: traverse, no write); --check <dir>... verify the 9997 entry
 * (0 ok, 1 not).
 *
 * Race with vold. vold writes its own default ACL after the module's scripts (the
 * framework orders CE prep; post-fs-data only does DE, vold-16/FsCrypt.cpp:657, and
 * package dirs appear at any time). Each setxattr(system.posix_acl_default) replaces
 * the ACL wholesale, wiping our 9997 entry (vold::SetDefaultAcl builds from scratch,
 * vold-16/Utils.cpp:142): /data/media/<user> (FsCrypt.cpp:1027, 1023 entry);
 * .../Android/obb (Utils.cpp:1889, no entry); .../Android/{data,obb,media}/<pkg>
 * (Utils.cpp:406, package uid entry). A dir created there by a process the libc hooks
 * do not cover (vold, MTP, a root daemon) inherits the wrong entry and stays invisible
 * to apps. Closed not by storage-fix but by tools/vold-noacl.c, which neutralises the
 * one setxattr call behind vold's rewrite (SetDefaultAcl then returns OK without
 * writing) so our ACLs hold; installed from post-fs-data.sh and service.sh.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#define XATTR_ACL_ACCESS "system.posix_acl_access"
#define XATTR_ACL_DEFAULT "system.posix_acl_default"

/* linux/include/uapi/linux/posix_acl_xattr.h */
#define POSIX_ACL_XATTR_VERSION 0x0002
#define ACL_USER_OBJ 0x01
#define ACL_USER 0x02
#define ACL_GROUP_OBJ 0x04
#define ACL_GROUP 0x08
#define ACL_MASK 0x10
#define ACL_OTHER 0x20

/* android_filesystem_config.h: shared group of all apps in a profile */
#define AID_EVERYBODY 9997

/* ACL entries we will parse; neither vold nor we write more than five. */
#define ACL_MAX_ENTRIES 32

struct acl_entry {
    uint16_t e_tag;
    uint16_t e_perm;
    uint32_t e_id;
};

static unsigned long stat_dirs, stat_files, stat_skipped, stat_errors;

/* Read an ACL into entries: >=0 count, -1 error (errno set), -2 no attribute
 * (not an error for the caller — just "no ACL set"). */
static int acl_read(const char *path, const char *name, struct acl_entry *out, size_t maxn) {
    uint8_t buf[sizeof(uint32_t) + ACL_MAX_ENTRIES * sizeof(struct acl_entry)];

    ssize_t len = getxattr(path, name, buf, sizeof(buf));
    if (len < 0) return errno == ENODATA ? -2 : -1;
    if (len < (ssize_t)sizeof(uint32_t)) {
        errno = EINVAL;
        return -1;
    }

    uint32_t version;
    memcpy(&version, buf, sizeof(version));
    if (version != POSIX_ACL_XATTR_VERSION) {
        errno = EINVAL;
        return -1;
    }

    size_t n = ((size_t)len - sizeof(uint32_t)) / sizeof(struct acl_entry);
    if (n > maxn) n = maxn;
    memcpy(out, buf + sizeof(uint32_t), n * sizeof(struct acl_entry));
    return (int)n;
}

/* Is there a named 9997 entry, and is it not cut by the mask? The mask decides
 * how much of the named entry a process actually gets; it comes from the dir mode. */
static int acl_allows_everybody(const char *path, const char *name, uint16_t *perm_out) {
    struct acl_entry e[ACL_MAX_ENTRIES];

    int n = acl_read(path, name, e, ACL_MAX_ENTRIES);
    if (n <= 0) return 0;

    uint16_t perm = 0, mask = 0;
    int have_mask = 0;

    for (int i = 0; i < n; i++) {
        if (e[i].e_tag == ACL_GROUP && e[i].e_id == AID_EVERYBODY) {
            perm = e[i].e_perm;
        } else if (e[i].e_tag == ACL_MASK) {
            mask = e[i].e_perm;
            have_mask = 1;
        }
    }

    if (perm == 0) return 0;
    if (have_mask && (perm & ~mask) != 0) return 0;
    if (perm_out) *perm_out = perm;
    return 1;
}

/* Build a 5-entry ACL like vold::SetDefaultAcl (vold-16/Utils.cpp:142): mode sets
 * owner/group/other; the 9997 named entry gets group rights, which also go into
 * the mask (otherwise the mask would null the grant). */
static int acl_apply(const char *path, const char *name, mode_t mode) {
    struct acl_entry e[5];
    const uint16_t group_perm = (mode & S_IRWXG) >> 3;
    size_t n = 0;

    e[n].e_tag = ACL_USER_OBJ;
    e[n].e_perm = (mode & S_IRWXU) >> 6;
    e[n].e_id = (uint32_t)-1;
    n++;
    e[n].e_tag = ACL_GROUP_OBJ;
    e[n].e_perm = group_perm;
    e[n].e_id = (uint32_t)-1;
    n++;
    e[n].e_tag = ACL_GROUP;
    e[n].e_perm = group_perm;
    e[n].e_id = AID_EVERYBODY;
    n++;
    e[n].e_tag = ACL_MASK;
    e[n].e_perm = group_perm;
    e[n].e_id = 0;
    n++;
    e[n].e_tag = ACL_OTHER;
    e[n].e_perm = mode & S_IRWXO;
    e[n].e_id = 0;
    n++;

    uint8_t buf[sizeof(uint32_t) + 5 * sizeof(struct acl_entry)];
    const uint32_t version = POSIX_ACL_XATTR_VERSION;
    memcpy(buf, &version, sizeof(version));
    memcpy(buf + sizeof(version), e, n * sizeof(struct acl_entry));
    const size_t len = sizeof(version) + n * sizeof(struct acl_entry);

    return setxattr(path, name, buf, len, 0);
}

/*
 * S_IRWXO (the "other" bits) is DELIBERATELY dropped in all three functions below.
 * Access is granted by the named 9997 ACL entry, not by the other bits: keeping
 * the on-disk other bits would make boot-fixed objects differ from app-created
 * ones (src/hook_libc.cpp's acl_build writes ACL_OTHER=0) and would let "other"
 * bypass the 9997 entry — wider than Android 10, where sdcardfs admitted only
 * group 9997. That is why the calls below pass already-zeroed other bits, so
 * `mode & S_IRWXO` in acl_apply yields 0; acl_apply itself stays an exact replica
 * of vold::SetDefaultAcl (vold-16/Utils.cpp:142), which is always called with 0770.
 */

/* Dir: keep owner, group gets rwx. */
static mode_t dir_mode(mode_t m) { return (m & S_IRWXU) | S_IRWXG; }

/* File: owner as-is, group rw, plus x if it was executable. */
static mode_t file_mode(mode_t m) {
    return (m & S_IRWXU) | (m & S_IRWXG) | S_IRGRP | S_IWGRP;
}

/* Volume root: group r-x only — traverse but do not write. */
static mode_t traverse_mode(mode_t m) {
    return (m & S_IRWXU) | (m & S_IRWXG) | S_IRGRP | S_IXGRP;
}

static int fix_dir(const char *path, mode_t m, int traverse_only) {
    const mode_t want = traverse_only ? traverse_mode(m) : dir_mode(m);
    int bad = 0;

    if (acl_apply(path, XATTR_ACL_ACCESS, want) != 0) bad = 1;
    if (acl_apply(path, XATTR_ACL_DEFAULT, want) != 0) bad = 1;

    if (bad) {
        fprintf(stderr, "storage-fix: %s: %s\n", path, strerror(errno));
        stat_errors++;
        return -1;
    }
    stat_dirs++;
    return 0;
}

static int fix_file(const char *path, mode_t m) {
    if (acl_apply(path, XATTR_ACL_ACCESS, file_mode(m)) != 0) {
        fprintf(stderr, "storage-fix: %s: %s\n", path, strerror(errno));
        stat_errors++;
        return -1;
    }
    stat_files++;
    return 0;
}

static void walk(const char *dir) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        fprintf(stderr, "storage-fix: %s: %s\n", dir, strerror(errno));
        stat_errors++;
        return;
    }

    const size_t base = strlen(dir);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        char *child = malloc(base + strlen(de->d_name) + 2);
        if (child == NULL) {
            stat_errors++;
            continue;
        }
        sprintf(child, "%s/%s", dir, de->d_name);

        struct stat st;
        if (lstat(child, &st) != 0) {
            fprintf(stderr, "storage-fix: %s: %s\n", child, strerror(errno));
            stat_errors++;
        } else if (S_ISLNK(st.st_mode)) {
            stat_skipped++; /* no ACL on a symlink, and none is needed */
        } else if (S_ISDIR(st.st_mode)) {
            fix_dir(child, st.st_mode, 0);
            walk(child);
        } else if (S_ISREG(st.st_mode)) {
            fix_file(child, st.st_mode);
        } else {
            stat_skipped++;
        }
        free(child);
    }
    closedir(d);
}

static void perm_str(uint16_t p, char out[4]) {
    out[0] = (p & 4) ? 'r' : '-';
    out[1] = (p & 2) ? 'w' : '-';
    out[2] = (p & 1) ? 'x' : '-';
    out[3] = '\0';
}

static int check_paths(int argc, char **argv, int first) {
    int bad = 0;

    for (int i = first; i < argc; i++) {
        struct stat st;
        if (lstat(argv[i], &st) != 0) {
            printf("ОШИБКА %s: %s\n", argv[i], strerror(errno));
            bad = 1;
            continue;
        }

        const int is_dir = S_ISDIR(st.st_mode);
        uint16_t a = 0, d = 0;
        const int ok_a = acl_allows_everybody(argv[i], XATTR_ACL_ACCESS, &a);
        const int ok_d = !is_dir || acl_allows_everybody(argv[i], XATTR_ACL_DEFAULT, &d);

        char sa[4], sd[4];
        perm_str(a, sa);
        perm_str(d, sd);

        printf("%s %s  access=%s default=%s\n", (ok_a && ok_d) ? "ОК  " : "НЕТ ",
               argv[i], ok_a ? sa : "нет 9997", is_dir ? (ok_d ? sd : "нет 9997") : "—");

        if (!ok_a || !ok_d) bad = 1;
    }

    return bad;
}

static int usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--traverse] <dir>...\n"
            "       %s --check <dir>...\n",
            argv0, argv0);
    return 2;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--check") == 0) {
        if (argc <= 2) return usage(argv[0]);
        return check_paths(argc, argv, 2);
    }

    int traverse_only = 0;
    int first = 1;

    if (argc > 1 && strcmp(argv[1], "--traverse") == 0) {
        traverse_only = 1;
        first = 2;
    }
    if (argc <= first) return usage(argv[0]);

    for (int i = first; i < argc; i++) {
        const char *root = argv[i];
        struct stat st;
        if (lstat(root, &st) != 0) {
            fprintf(stderr, "storage-fix: %s: %s\n", root, strerror(errno));
            stat_errors++;
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "storage-fix: %s: не каталог\n", root);
            stat_errors++;
            continue;
        }
        fix_dir(root, st.st_mode, traverse_only);
        if (!traverse_only) walk(root);
    }

    printf("storage-fix: каталогов=%lu файлов=%lu пропущено=%lu ошибок=%lu\n", stat_dirs,
           stat_files, stat_skipped, stat_errors);
    return stat_errors == 0 ? 0 : 1;
}
