/*
 * storage-fix — permissions on the raw /data/media tree when sdcardfs is not the
 *               one handing them out.
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
 * (0 ok, 1 not); --app-dirs <dir>... add the per-package entry to
 * Android/{data,obb,media}/<pkg> (see acl_apply_extra and fix_app_dirs);
 * --dump <path>... print mode/owner and the ACLs (diagnostics; the default ACL is
 * printed only when it differs from the access one — see dump_path);
 * --probe <uid> <path> run an access check as that uid (diagnostics).
 *
 * The two diagnostic verbs exist because the failure they are for cannot be seen
 * from outside: an app that gets EACCES on its own Android/data/<pkg> reports only
 * the errno, and `ls`/`stat` show neither the ACL nor which path component denied.
 * --dump prints what the kernel will actually act on, --probe reproduces the app's
 * own syscall (uid AND supplementary groups — `su <uid>` drops the groups, so a
 * check through it is a false negative by construction). Both are read-only apart
 * from --probe, which creates and immediately removes one file.
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
 *
 * Suppressing vold's write also throws away the ONE thing that write got right: the
 * per-package entry for the app's own uid (vold-11/Utils.cpp:321,:398). That is why
 * --app-dirs exists — the module now supplies the entry vold would have written, and
 * keeps its own 9997 grant on top instead of choosing between them. The alternative
 * (leave vold's write alone and merge 9997 into its buffer) would mean a second
 * generated arm64 handler living inside vold for no gain: ours is a strict superset.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
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

/* linux/include/uapi/linux/posix_acl_xattr.h. ACL_USER (0x02) is never written
 * here — access is granted through the named GROUP entry for 9997 — but --dump
 * does read it back: vold writes per-package entries as additional *groups*
 * (Utils.cpp:321), yet an entry of that tag may still appear in a tree the module
 * did not create, and a dump that silently skipped it would misreport the ACL. */
#define POSIX_ACL_XATTR_VERSION 0x0002
#define ACL_USER_OBJ 0x01
#define ACL_USER 0x02
#define ACL_GROUP_OBJ 0x04
#define ACL_GROUP 0x08
#define ACL_MASK 0x10
#define ACL_OTHER 0x20

/* android_filesystem_config.h: shared group of all apps in a profile */
#define AID_EVERYBODY 9997

/* android_filesystem_config.h: AID_APP_START — the first app uid. Used only to
 * tell a per-package directory (owned by the app) from one owned by root/system,
 * where the named entry below would be meaningless. */
#define AID_APP_START 10000

/* android_filesystem_config.h: the groups every app process carries besides its
 * own. --probe reproduces them so the check sees what the kernel would see for a
 * real app; a check with only the uid would miss the 9997 grant entirely. */
#define AID_EXT_DATA_RW 1078
#define AID_EXT_OBB_RW 1079
#define AID_INET 3003

/* ACL entries we will parse; neither vold nor we write more than five. */
#define ACL_MAX_ENTRIES 32

struct acl_entry {
    uint16_t e_tag;
    uint16_t e_perm;
    uint32_t e_id;
};

static unsigned long stat_errors;

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

/* The same ACL plus a named entry for the package's own uid — AOSP's
 * additionalGids.push_back(uid) (vold-11/Utils.cpp:321, used at :398). Its
 * comment names the reason exactly: an app must keep access to the files in its
 * own Android/data/<pkg> "even if they are created by other processes".
 *
 * Our single 9997 entry does not cover that case. An object created inside the
 * app's directory by another uid (a root helper, an installer, MTP, vold) inherits
 * no entry for the app, and if the creating process asked for a restrictive mode
 * the mask cuts the grant as well (posix_acl_create_masq on 0600/0700 — the failure
 * the project itself had already named in docs/android-11-design.md). With the
 * app's own uid present the app is matched by name, and the mode the creator used
 * stops deciding whether it gets in.
 *
 * tools/vold-noacl stops vold from writing its own version of this ACL (it turns
 * the one setxattr call behind SetDefaultAcl into a no-op), so this pass is what
 * supplies it. Keeping 9997 as well is deliberate: vold's own buffer carries the
 * uid and drops 9997, ours carries both, so the module's cross-app model survives
 * the fix instead of being traded for it. ACL_GROUP entries have to be sorted by
 * id (posix_acl_valid), and an app uid is always above 9997, so the order below
 * is the required one. */
static int acl_apply_extra(const char *path, const char *name, mode_t mode, uint32_t extra_uid) {
    struct acl_entry e[6];
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
    if (extra_uid != AID_EVERYBODY) {
        e[n].e_tag = ACL_GROUP;
        e[n].e_perm = group_perm;
        e[n].e_id = extra_uid;
        n++;
    }
    e[n].e_tag = ACL_MASK;
    e[n].e_perm = group_perm;
    e[n].e_id = 0;
    n++;
    e[n].e_tag = ACL_OTHER;
    e[n].e_perm = mode & S_IRWXO;
    e[n].e_id = 0;
    n++;

    uint8_t buf[sizeof(uint32_t) + 6 * sizeof(struct acl_entry)];
    const uint32_t version = POSIX_ACL_XATTR_VERSION;
    memcpy(buf, &version, sizeof(version));
    memcpy(buf + sizeof(version), e, n * sizeof(struct acl_entry));
    const size_t len = sizeof(version) + n * sizeof(struct acl_entry);

    return setxattr(path, name, buf, len, 0);
}

/*
 * The "other" bits (S_IRWXO) are kept exactly where the on-disk mode carries
 * them, and nowhere else. That is not a blanket widening: every dir and file
 * under /data/media is 0770/0660, so `m & S_IRWXO` is 0 for all of them and the
 * ACL is unchanged. The only objects whose mode has other bits set are the four
 * Android* levels, which vold prepares as 02771 (vold-11/Utils.cpp:1588,
 * PrepareAndroidDirs, mode = S_IRWXU|S_IRWXG|S_IXOTH|S_ISGID) — drwxrws--x,
 * "traverse but do not read".
 *
 * Dropping those bits was a regression, not a hardening. Android 10 showed
 * /storage/emulated/0/Android as exactly drwxrws--x, and --x is traversal only.
 * It matters because the mode bits are the ONLY thing a consumer that does not
 * read POSIX ACLs can act on — above all the FUSE view of MediaProvider, which
 * is still on the path whenever tools/vold-fusefs did not take (the state the
 * crDroid A11 log records, docs/android-11-crdroid-log.md). With other stripped
 * that view denies the whole /storage/emulated/0/Android subtree, and an app
 * cannot create a file in its own Android/data/<pkg> — the EACCES reported by
 * two applications on 2026-10-09 (docs/android-11-app-access-fix.md). Restoring
 * --x there is restoring Android 10, not exceeding it.
 */

/* Dir: keep owner, group gets rwx, other as the mode had it. */
static mode_t dir_mode(mode_t m) { return (m & S_IRWXU) | S_IRWXG | (m & S_IRWXO); }

/* File: owner as-is, group rw, plus x if it was executable. */
static mode_t file_mode(mode_t m) {
    return (m & S_IRWXU) | (m & S_IRWXG) | S_IRGRP | S_IWGRP | (m & S_IRWXO);
}

/* Volume root: group r-x only — traverse but do not write. */
static mode_t traverse_mode(mode_t m) {
    return (m & S_IRWXU) | (m & S_IRWXG) | S_IRGRP | S_IXGRP | (m & S_IRWXO);
}

static int fix_dir(const char *path, mode_t m, int traverse_only) {
    const mode_t want = traverse_only ? traverse_mode(m) : dir_mode(m);
    int bad = 0;

    if (acl_apply(path, XATTR_ACL_ACCESS, want) != 0) bad = 1;
    if (acl_apply(path, XATTR_ACL_DEFAULT, want) != 0) bad = 1;

    if (bad) {
        stat_errors++;
        return -1;
    }
    return 0;
}

static int fix_file(const char *path, mode_t m) {
    if (acl_apply(path, XATTR_ACL_ACCESS, file_mode(m)) != 0) {
        stat_errors++;
        return -1;
    }
    return 0;
}

/* <parent>/<pkg> with the per-package entry, for the mode helpers above (hence
 * the position: dir_mode is defined just before fix_dir). */
static int fix_app_dir(const char *path, mode_t m, uint32_t uid) {
    const mode_t want = dir_mode(m);
    int bad = 0;

    if (acl_apply_extra(path, XATTR_ACL_ACCESS, want, uid) != 0) bad = 1;
    if (acl_apply_extra(path, XATTR_ACL_DEFAULT, want, uid) != 0) bad = 1;

    if (bad) {
        stat_errors++;
        return -1;
    }
    return 0;
}

/* One shallow level: <parent>/<pkg>, for parent = Android/{data,obb,media}.
 *
 * The depth is capped on purpose. The per-package entry is needed exactly where
 * vold would have written it — depth 0 of the application-specific directory
 * (vold-11/Utils.cpp:398, called only when depth == 0) — and everything deeper
 * inherits the default ACL from there. A recursive pass would cost a full walk
 * of every app's data for nothing. Idempotent, so service.sh may repeat it.
 *
 * The uid is taken from the directory itself rather than from PackageManager:
 * vold creates <pkg> with uid = appUid (PrepareDirWithProjectId, vold-11/Utils.cpp:390),
 * so st_uid here IS the app's uid, not an approximation of it. A directory owned
 * by root or system (no app) is skipped — a named entry for it would grant nothing. */
static void fix_app_dirs(const char *parent) {
    DIR *d = opendir(parent);
    if (d == NULL) {
        stat_errors++;
        return;
    }

    const size_t base = strlen(parent);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char *child = malloc(base + strlen(de->d_name) + 2);
        if (child == NULL) {
            stat_errors++;
            continue;
        }
        sprintf(child, "%s/%s", parent, de->d_name);

        struct stat st;
        if (lstat(child, &st) != 0) {
            stat_errors++;
        } else if (S_ISDIR(st.st_mode) && st.st_uid >= AID_APP_START) {
            fix_app_dir(child, st.st_mode, (uint32_t)st.st_uid);
        }
        free(child);
    }
    closedir(d);
}

static void walk(const char *dir) {
    DIR *d = opendir(dir);
    if (d == NULL) {
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
            stat_errors++;
        } else if (S_ISLNK(st.st_mode)) {
            /* no ACL on a symlink, and none is needed */
        } else if (S_ISDIR(st.st_mode)) {
            fix_dir(child, st.st_mode, 0);
            walk(child);
        } else if (S_ISREG(st.st_mode)) {
            fix_file(child, st.st_mode);
        }
        free(child);
    }
    closedir(d);
}

static int check_paths(int argc, char **argv, int first) {
    int bad = 0;

    for (int i = first; i < argc; i++) {
        struct stat st;
        if (lstat(argv[i], &st) != 0) {
            bad = 1;
            continue;
        }

        const int is_dir = S_ISDIR(st.st_mode);
        const int ok_a = acl_allows_everybody(argv[i], XATTR_ACL_ACCESS, NULL);
        const int ok_d = !is_dir || acl_allows_everybody(argv[i], XATTR_ACL_DEFAULT, NULL);

        if (!ok_a || !ok_d) bad = 1;
    }

    return bad;
}

/* ------------------------------------------------------------- diagnostics */

static void perm_text(uint16_t perm, char *out) {
    out[0] = (perm & 4) ? 'r' : '-';
    out[1] = (perm & 2) ? 'w' : '-';
    out[2] = (perm & 1) ? 'x' : '-';
    out[3] = '\0';
}

/* Один ACL строкой, без подписи. 0 — есть, -2 — записи нет, -1 — ошибка. */
static int format_acl(const char *path, const char *name, char *out, size_t outlen) {
    struct acl_entry e[ACL_MAX_ENTRIES];
    char p[4], item[64];

    int n = acl_read(path, name, e, ACL_MAX_ENTRIES);
    if (n == -2) return -2;
    if (n < 0) return -1;

    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        perm_text(e[i].e_perm, p);
        switch (e[i].e_tag) {
            case ACL_USER_OBJ:  snprintf(item, sizeof item, " USER_OBJ=%s", p); break;
            case ACL_USER:      snprintf(item, sizeof item, " USER:%u=%s", e[i].e_id, p); break;
            case ACL_GROUP_OBJ: snprintf(item, sizeof item, " GROUP_OBJ=%s", p); break;
            case ACL_GROUP:     snprintf(item, sizeof item, " GROUP:%u=%s", e[i].e_id, p); break;
            case ACL_MASK:      snprintf(item, sizeof item, " MASK=%s", p); break;
            case ACL_OTHER:     snprintf(item, sizeof item, " OTHER=%s", p); break;
            default:            snprintf(item, sizeof item, " TAG:0x%x:%u=%s", e[i].e_tag, e[i].e_id, p); break;
        }
        size_t used = strlen(out);
        if (used + strlen(item) + 1 > outlen) break;
        memcpy(out + used, item, strlen(item) + 1);
    }
    return 0;
}

/* Строка ACL — ровно те записи, на которые смотрит ядро, включая маску: маска
 * молча обнуляет именованную запись, и в `ls -l` её не видно вовсе.
 *
 * `default:` печатается только когда отличается от `access:`. В снимке --dump
 * идёт по десяткам каталогов, и совпадение там — правило, а не случайность:
 * печатать обе строки значило бы дублировать одно и то же в каждом каталоге.
 * Отсутствие записи при этом остаётся видимым: это `none`, а не пропуск. */
static void dump_path(const char *path) {
    struct stat st;
    char a[512], d[512];

    printf("DUMP %s\n", path);
    if (lstat(path, &st) != 0) {
        printf("  stat    : FAIL errno=%d (%s)\n", errno, strerror(errno));
        stat_errors++;
        return;
    }

    printf("  stat    : mode=%04o type=%s uid=%u gid=%u\n", st.st_mode & 07777,
           S_ISDIR(st.st_mode) ? "dir" : (S_ISREG(st.st_mode) ? "file" : "other"),
           (unsigned)st.st_uid, (unsigned)st.st_gid);

    int ra = format_acl(path, XATTR_ACL_ACCESS, a, sizeof a);
    if (ra == -2)        printf("  access  : none\n");
    else if (ra < 0)   { printf("  access  : ERROR errno=%d (%s)\n", errno, strerror(errno)); stat_errors++; }
    else                 printf("  access  :%s\n", a);

    if (!S_ISDIR(st.st_mode)) return;

    int rd = format_acl(path, XATTR_ACL_DEFAULT, d, sizeof d);
    if (rd == -2)        printf("  default : none\n");
    else if (rd < 0)   { printf("  default : ERROR errno=%d (%s)\n", errno, strerror(errno)); stat_errors++; }
    else if (ra == 0 && strcmp(a, d) == 0) /* повтор access — не печатаем */ ;
    else                 printf("  default :%s\n", d);
}

/* Группы живого процесса с этим uid — то, что устройство реально выдаёт
 * приложению. Своим набором проба была бы оптимистичной: она подставляла бы
 * 9997 даже там, где процесс его не имеет, и «всё хорошо» ничего не значило бы.
 * Если процесса с таким uid нет, берём набор обычного приложения и говорим об
 * этом в выводе. */
static int groups_from_proc(uid_t uid, gid_t *out, int max) {
    DIR *d = opendir("/proc");
    if (d == NULL) return 0;

    int n = 0;
    struct dirent *de;
    char path[64], line[512];

    while (n == 0 && (de = readdir(d)) != NULL) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;

        snprintf(path, sizeof(path), "/proc/%s/status", de->d_name);
        FILE *f = fopen(path, "r");
        if (f == NULL) continue;

        uid_t p_uid = (uid_t)-1;
        int have_groups = 0;
        while (fgets(line, sizeof(line), f) != NULL) {
            if (strncmp(line, "Uid:", 4) == 0) {
                unsigned long v = 0;
                if (sscanf(line + 4, "%lu", &v) == 1) p_uid = (uid_t)v;
            } else if (strncmp(line, "Groups:", 7) == 0) {
                const char *p = line + 7;
                while (n < max) {
                    while (*p == ' ' || *p == '\t') p++;
                    if (*p < '0' || *p > '9') break;
                    char *end = NULL;
                    const unsigned long g = strtoul(p, &end, 10);
                    if (end == p) break;
                    out[n++] = (gid_t)g;
                    p = end;
                }
                have_groups = 1;
                break;
            }
        }
        fclose(f);

        if (!(p_uid == uid && have_groups)) n = 0;
    }

    closedir(d);
    return n;
}

/* Reproduce the access an app process makes: the same uid AND the same
 * supplementary groups (9997 among them). Two things this buys over reading the
 * dump by eye:
 *   - the groups. `su <uid>` drops them, so a check through su denies an app that
 *     the kernel would let in — a false negative that hides the real cause.
 *   - the component. An EACCES on a deep path does not say which directory
 *     denied; walking the prefixes names it.
 * Nothing is left behind: the file is created, written, and removed. */
static int probe_access(unsigned long uid_arg, const char *path) {
    const uid_t uid = (uid_t)uid_arg;
    gid_t groups[16];
    int ngroups = groups_from_proc(uid, groups, 16);
    const char *source = "из живого процесса";

    if (ngroups == 0) {
        groups[0] = (gid_t)uid;
        groups[1] = AID_EVERYBODY;
        groups[2] = AID_EXT_DATA_RW;
        groups[3] = AID_EXT_OBB_RW;
        groups[4] = AID_INET;
        ngroups = 5;
        source = "набор обычного приложения (живого процесса с этим uid нет)";
    }

    printf("PROBE uid=%lu path=%s\n", uid_arg, path);
    printf("  группы (%s):", source);
    for (int i = 0; i < ngroups; i++) printf(" %u", (unsigned)groups[i]);
    printf("\n");

    if (setgroups((size_t)ngroups, groups) != 0) {
        printf("  setgroups: FAIL errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }
    if (setresgid(uid, uid, uid) != 0) {
        printf("  setresgid: FAIL errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }
    if (setresuid(uid, uid, uid) != 0) {
        printf("  setresuid: FAIL errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }

    char buf[4096];
    const size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) {
        printf("  path: негодный (%zu байт)\n", len);
        return 1;
    }
    memcpy(buf, path, len + 1);

    int denied = 0;
    for (size_t i = 1; i < len; i++) {
        if (buf[i] != '/') continue;
        buf[i] = '\0';
        struct stat st;
        if (stat(buf, &st) != 0) {
            printf("  component %s: FAIL errno=%d (%s)\n", buf, errno, strerror(errno));
            denied = 1;
            break;
        }
        buf[i] = '/';
    }

    if (!denied) {
        /* Если файл уже есть и это обычный файл — проверяем ровно то, что делает
         * приложение: открытие на запись. Ничего не пишем и не удаляем, данные
         * не трогаются (O_APPEND без write). Это случай из логов пользователя:
         * `FileOutputStream` на существующий cache/am.log даёт EACCES, и без
         * этой ветки проба отказывалась бы и не говорила ничего.
         * Всё остальное (каталог, ссылка, fifo) — отказ: чужое не трогаем. */
        struct stat existing;
        if (lstat(path, &existing) == 0) {
            if (S_ISREG(existing.st_mode)) {
                int fd = open(path, O_WRONLY | O_APPEND);
                if (fd < 0) {
                    printf("  open %s (существующий, O_APPEND): FAIL errno=%d (%s)\n",
                           path, errno, strerror(errno));
                    return 1;
                }
                close(fd);
                printf("  open %s (существующий, O_APPEND): OK (не изменён)\n", path);
                return 0;
            }
            printf("  refuse: %s уже существует (не обычный файл) — не трогаю\n", path);
            return 1;
        }

        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0660);
        if (fd < 0) {
            printf("  open %s: FAIL errno=%d (%s)\n", path, errno, strerror(errno));
            denied = 1;
        } else {
            const ssize_t w = write(fd, "unfuse-probe\n", 13);
            close(fd);
            unlink(path);
            printf("  open %s: OK (write=%ld)\n", path, (long)w);
        }
    }

    return denied;
}

static int usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--traverse] <dir>...\n"
            "       %s --check <dir>...\n"
            "       %s --app-dirs <dir>...\n"
            "       %s --dump <path>...\n"
            "       %s --probe <uid> <path>\n",
            argv0, argv0, argv0, argv0, argv0);
    return 2;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--check") == 0) {
        if (argc <= 2) return usage(argv[0]);
        return check_paths(argc, argv, 2);
    }

    /* <dir> here is an Android/{data,obb,media} directory; the pass is one level
     * deep (see fix_app_dirs). Called from module/storage.sh, which only passes
     * directories that exist. */
    if (argc > 1 && strcmp(argv[1], "--app-dirs") == 0) {
        if (argc <= 2) return usage(argv[0]);
        for (int i = 2; i < argc; i++) fix_app_dirs(argv[i]);
        return stat_errors == 0 ? 0 : 1;
    }

    /* Diagnostics only; they change nothing. Driven by module/diag.sh. */
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        if (argc <= 2) return usage(argv[0]);
        for (int i = 2; i < argc; i++) dump_path(argv[i]);
        return stat_errors == 0 ? 0 : 1;
    }

    if (argc > 1 && strcmp(argv[1], "--probe") == 0) {
        if (argc <= 3) return usage(argv[0]);
        char *end = NULL;
        const unsigned long uid = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0') return usage(argv[0]);
        return probe_access(uid, argv[3]);
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
            stat_errors++;
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            stat_errors++;
            continue;
        }
        fix_dir(root, st.st_mode, traverse_only);
        if (!traverse_only) walk(root);
    }

    return stat_errors == 0 ? 0 : 1;
}
