/*
 * aclprobe — experiment harness for the sdcardfs-free storage fallback.
 *
 * Q1: with a POSIX *default* ACL on a directory, does a process running with
 *     umask 0077 still produce files other apps can open read-write?
 *     (vold-16/FsCrypt.cpp:1023 claims it does.)
 * Q2: does a named ACL_GROUP entry for AID_EVERYBODY (9997) give every app
 *     access, given that every app carries 9997 in its supplementary groups?
 * Q3: what does it take to fix *pre-existing* entries (the tree is 0670/2770
 *     gid=1023 and apps are not in group 1023)?
 * Q4: does a later chmod() destroy a named ACL entry?
 *
 * Run as root on the device. All mutations happen inside /data/media/0/.aclprobe,
 * which the program creates and removes itself.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#define XATTR_ACL_DEFAULT "system.posix_acl_default"
#define XATTR_ACL_ACCESS "system.posix_acl_access"

#define ACL_USER_OBJ 0x01
#define ACL_USER 0x02
#define ACL_GROUP_OBJ 0x04
#define ACL_GROUP 0x08
#define ACL_MASK 0x10
#define ACL_OTHER 0x20
#define POSIX_ACL_XATTR_VERSION 0x0002

#define AID_MEDIA_RW 1023
#define AID_EVERYBODY 9997

/* Identities sampled from /proc/<pid>/status on the device. */
static const uid_t APP_UID = 10398;
static const gid_t APP_GID = 10398;
static const gid_t APP_GROUPS[] = {1077, 3001, 3002, 3003, 9997, 20398, 50398};
static const uid_t APP2_UID = 10400;
static const gid_t APP2_GID = 10400;
static const gid_t APP2_GROUPS[] = {1077, 3001, 3002, 3003, 9997, 20400, 50400};
static const uid_t MP_UID = AID_MEDIA_RW;
static const gid_t MP_GID = AID_MEDIA_RW;
static const gid_t MP_GROUPS[] = {AID_MEDIA_RW, 3003};

struct acl_entry {
    uint16_t e_tag;
    uint16_t e_perm;
    uint32_t e_id;
};

static const char *tag_name(uint16_t t) {
    switch (t) {
        case ACL_USER_OBJ: return "USER_OBJ";
        case ACL_USER: return "USER";
        case ACL_GROUP_OBJ: return "GROUP_OBJ";
        case ACL_GROUP: return "GROUP";
        case ACL_MASK: return "MASK";
        case ACL_OTHER: return "OTHER";
        default: return "?";
    }
}

static void perm_str(uint16_t p, char out[4]) {
    out[0] = (p & 4) ? 'r' : '-';
    out[1] = (p & 2) ? 'w' : '-';
    out[2] = (p & 1) ? 'x' : '-';
    out[3] = 0;
}

static void dump_acl(const char *path, const char *name) {
    uint8_t buf[512];
    ssize_t n = getxattr(path, name, buf, sizeof(buf));
    if (n < 0) {
        printf("      %-22s <%s>\n", name, strerror(errno));
        return;
    }
    uint32_t ver;
    memcpy(&ver, buf, sizeof(ver));
    printf("      %-22s v%u", name, ver);
    int cnt = (int)((n - (ssize_t)sizeof(ver)) / (ssize_t)sizeof(struct acl_entry));
    for (int i = 0; i < cnt; i++) {
        struct acl_entry e;
        memcpy(&e, buf + sizeof(ver) + i * sizeof(e), sizeof(e));
        char ps[4];
        perm_str(e.e_perm, ps);
        printf("  %s(id=%u,%s)", tag_name(e.e_tag), e.e_id, ps);
    }
    printf("\n");
}

/* Same layout vold builds in SetDefaultAcl() (vold-16/Utils.cpp:142-202). */
static int write_acl(const char *path, const char *name, mode_t mode, const gid_t *extra,
                     int nextra) {
    size_t entries = 3 + (nextra > 0 ? (size_t)nextra + 1 : 0);
    size_t size = sizeof(uint32_t) + entries * sizeof(struct acl_entry);
    uint8_t *buf = calloc(1, size);
    if (!buf) return -1;
    uint32_t ver = POSIX_ACL_XATTR_VERSION;
    memcpy(buf, &ver, sizeof(ver));
    struct acl_entry *e = (struct acl_entry *)(buf + sizeof(ver));
    int i = 0;
    e[i].e_tag = ACL_USER_OBJ;
    e[i].e_perm = (mode & S_IRWXU) >> 6;
    e[i].e_id = (uint32_t)-1;
    i++;
    e[i].e_tag = ACL_GROUP_OBJ;
    e[i].e_perm = (mode & S_IRWXG) >> 3;
    e[i].e_id = (uint32_t)-1;
    i++;
    for (int k = 0; k < nextra; k++) {
        e[i].e_tag = ACL_GROUP;
        e[i].e_perm = (mode & S_IRWXG) >> 3;
        e[i].e_id = extra[k];
        i++;
    }
    if (nextra > 0) {
        e[i].e_tag = ACL_MASK;
        e[i].e_perm = (mode & S_IRWXG) >> 3;
        e[i].e_id = 0;
        i++;
    }
    e[i].e_tag = ACL_OTHER;
    e[i].e_perm = mode & S_IRWXO;
    e[i].e_id = 0;
    int r = setxattr(path, name, buf, size, 0);
    if (r != 0) printf("      setxattr(%s,%s) -> %s\n", path, name, strerror(errno));
    free(buf);
    return r;
}

static void stat_line(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        printf("      %-44s stat -> %s\n", path, strerror(errno));
        return;
    }
    printf("      %-44s mode=%04o uid=%u gid=%u\n", path, st.st_mode & 07777, st.st_uid, st.st_gid);
}

enum act {
    ACT_CREATE,
    ACT_MKDIR,
    ACT_OPEN_RDWR,
    ACT_OPEN_RDONLY,
    ACT_LIST,
    ACT_PROBE,
    ACT_CHMOD,
    ACT_CHOWN,
    ACT_STAT
};

/* Target of ACT_CHOWN; the forked child inherits them. */
static uid_t g_new_uid;
static gid_t g_new_gid;

struct task {
    enum act act;
    const char *path;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    const gid_t *groups;
    int ngroups;
    const char *label;
};

static void print_self(const char *tag) {
    char buf[4096];
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "Uid:", 4) || !strncmp(line, "Gid:", 4) || !strncmp(line, "Groups:", 7) ||
            !strncmp(line, "Umask:", 6) || !strncmp(line, "CapEff:", 7))
            printf("        [%s] %s\n", tag, line);
    }
}

/* Try a spread of operations and report raw errno numbers. */
static void probe_ops(const char *dir, const char *label) {
    print_self(label);
    char file[512];
    snprintf(file, sizeof(file), "%s/f_probe", dir);
    int r = access(dir, R_OK | W_OK | X_OK);
    printf("        access(dir, RWX)   -> %d (%s)\n", r ? errno : 0, r ? strerror(errno) : "OK");
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    printf("        open(dir, O_RDONLY)-> %d (%s)\n", fd < 0 ? errno : 0, fd < 0 ? strerror(errno) : "OK");
    if (fd >= 0) close(fd);
    fd = open(file, O_CREAT | O_RDWR, 0666);
    printf("        creat(dir/f, 0666) -> %d (%s)\n", fd < 0 ? errno : 0, fd < 0 ? strerror(errno) : "OK");
    if (fd >= 0) {
        if (write(fd, "x", 1) != 1) printf("        write(fd)          -> %d\n", errno);
        close(fd);
    }
}

static int run_as(enum act a, const char *path, mode_t mode, uid_t uid, gid_t gid,
                  const gid_t *groups, int ngroups, const char *label) {
    fflush(stdout);
    pid_t p = fork();
    if (p < 0) {
        perror("fork");
        return -1;
    }
    if (p == 0) {
        if (setgroups(ngroups, (gid_t *)groups) != 0 || setgid(gid) != 0 || setuid(uid) != 0) {
            printf("      [%s] setgroups/setuid -> %s\n", label, strerror(errno));
            _exit(2);
        }
        umask(0077); /* exactly what real app processes run with */
        int rc = 0;
        if (a == ACT_CREATE) {
            int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, mode);
            printf("      [%s uid=%u umask=077] create %-42s %04o -> %s\n", label, uid, path, mode,
                   fd < 0 ? strerror(errno) : "OK");
            if (fd >= 0) {
                if (write(fd, "x", 1) != 1) rc = 3;
                close(fd);
            } else {
                rc = 1;
            }
        } else if (a == ACT_MKDIR) {
            int r = mkdir(path, mode);
            printf("      [%s uid=%u umask=077] mkdir  %-42s %04o -> %s\n", label, uid, path, mode,
                   r != 0 ? strerror(errno) : "OK");
            rc = r != 0 ? 1 : 0;
        } else if (a == ACT_OPEN_RDWR) {
            int fd = open(path, O_RDWR);
            printf("      [%s uid=%u] open(O_RDWR) %-38s -> %s\n", label, uid, path,
                   fd < 0 ? strerror(errno) : "OK");
            if (fd >= 0)
                close(fd);
            else
                rc = 1;
        } else if (a == ACT_OPEN_RDONLY) {
            int fd = open(path, O_RDONLY);
            printf("      [%s uid=%u] open(O_RDONLY) %-36s -> %s\n", label, uid, path,
                   fd < 0 ? strerror(errno) : "OK");
            if (fd >= 0)
                close(fd);
            else
                rc = 1;
        } else if (a == ACT_CHMOD) {
            int r = chmod(path, mode);
            printf("      [%s uid=%u] chmod %-40s %04o -> %s\n", label, uid, path, mode,
                   r != 0 ? strerror(errno) : "OK");
            rc = r != 0 ? 1 : 0;
        } else if (a == ACT_CHOWN) {
            int r = chown(path, g_new_uid, g_new_gid);
            printf("      [%s uid=%u] chown %-38s uid=%u gid=%u -> %s\n", label, uid, path,
                   g_new_uid, g_new_gid, r != 0 ? strerror(errno) : "OK");
            rc = r != 0 ? 1 : 0;
        } else if (a == ACT_STAT) {
            struct stat st;
            if (stat(path, &st) != 0) {
                printf("      [%s uid=%u] stat %-39s -> %s\n", label, uid, path, strerror(errno));
                rc = 1;
            } else {
                printf("      [%s uid=%u] stat %-39s -> mode=%04o uid=%u gid=%u\n", label, uid, path,
                       st.st_mode & 07777, st.st_uid, st.st_gid);
            }
        } else if (a == ACT_LIST) {
            DIR *d = opendir(path);
            if (!d) {
                printf("      [%s uid=%u] opendir %-38s -> %s\n", label, uid, path, strerror(errno));
                rc = 1;
            } else {
                int n = 0;
                struct dirent *de;
                while ((de = readdir(d))) n++;
                closedir(d);
                printf("      [%s uid=%u] opendir %-38s -> OK (%d entries)\n", label, uid, path, n);
            }
        } else if (a == ACT_PROBE) {
            probe_ops(path, label);
        }
        fflush(stdout);
        _exit(rc);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

#define AS_APP(a, p, m) run_as(a, p, m, APP_UID, APP_GID, APP_GROUPS, 7, "app1")
#define AS_APP2(a, p, m) run_as(a, p, m, APP2_UID, APP2_GID, APP2_GROUPS, 7, "app2")
#define AS_MP(a, p, m) run_as(a, p, m, MP_UID, MP_GID, MP_GROUPS, 2, "mediaprov")

#define S "/data/media/0/.aclprobe"

/* Normalise one directory: grant named group 9997 access on the directory
 * itself (access ACL) and on everything created inside it (default ACL). */
static void grant_dir(const char *path, mode_t mode) {
    write_acl(path, XATTR_ACL_ACCESS, mode, (const gid_t[]){AID_EVERYBODY}, 1);
    write_acl(path, XATTR_ACL_DEFAULT, mode, (const gid_t[]){AID_EVERYBODY}, 1);
}

/* Normalise one file: access ACL only. */
static void grant_file(const char *path, mode_t mode) {
    write_acl(path, XATTR_ACL_ACCESS, mode, (const gid_t[]){AID_EVERYBODY}, 1);
}

#define D2 "/data/media/0/.aclprobe2"

/* Verify an already-normalised tree the way a real app would use it. */
static int cmd_verify(const char *dir) {
    char f[512];
    snprintf(f, sizeof(f), "%s/verify_by_app1", dir);
    printf("=== app1 (uid %u) on %s ===\n", APP_UID, dir);
    AS_APP(ACT_LIST, dir, 0);
    AS_APP(ACT_CREATE, f, 0666);
    stat_line(f);
    dump_acl(f, XATTR_ACL_ACCESS);
    printf("=== app2 (uid %u) opens the file app1 created ===\n", APP2_UID);
    AS_APP2(ACT_OPEN_RDWR, f, 0);
    AS_APP2(ACT_LIST, dir, 0);
    return 0;
}

/* Isolate: does an access ACL written by setxattr() onto an *existing* inode
 * take effect, or is the inode's cached ACL stale? */
static int cmd_dbg3(void) {
    printf("=== /data/media as app1, BEFORE ===\n");
    AS_APP(ACT_OPEN_RDWR, "/data/media", 0);
    AS_APP(ACT_LIST, "/data/media/0", 0);

    printf("\n=== write access ACL on /data/media (named 9997 r-x) ===\n");
    write_acl("/data/media", XATTR_ACL_ACCESS, 0550, (const gid_t[]){AID_EVERYBODY}, 1);
    dump_acl("/data/media", XATTR_ACL_ACCESS);
    stat_line("/data/media");
    AS_APP(ACT_OPEN_RDWR, "/data/media", 0);

    printf("\n=== write access ACL on /data/media/0 (named 9997 rwx) ===\n");
    write_acl("/data/media/0", XATTR_ACL_ACCESS, 02770, (const gid_t[]){AID_EVERYBODY}, 1);
    dump_acl("/data/media/0", XATTR_ACL_ACCESS);
    stat_line("/data/media/0");
    AS_APP(ACT_LIST, "/data/media/0", 0);

    printf("\n=== after sync + drop_caches ===\n");
    fflush(stdout);
    system("sync; echo 3 > /proc/sys/vm/drop_caches");
    AS_APP(ACT_OPEN_RDWR, "/data/media", 0);
    AS_APP(ACT_LIST, "/data/media/0", 0);
    AS_APP(ACT_LIST, "/data/media/0/Download", 0);

    printf("\n=== control: a directory created right now, ACL set immediately ===\n");
    system("rm -rf " S);
    mkdir(S, 0770);
    chown(S, AID_MEDIA_RW, AID_MEDIA_RW);
    chmod(S, 02770);
    grant_dir(S, 0770);
    stat_line(S);
    AS_APP(ACT_LIST, S, 0);
    AS_APP(ACT_CREATE, S "/f_now", 0666);
    stat_line(S "/f_now");
    dump_acl(S "/f_now", XATTR_ACL_ACCESS);
    AS_APP2(ACT_OPEN_RDWR, S "/f_now", 0);
    system("rm -rf " S);
    return 0;
}
static int cmd_dbg2(void) {
    system("rm -rf " D2);
    mkdir(D2, 0770);
    chown(D2, AID_MEDIA_RW, AID_MEDIA_RW);
    chmod(D2, 02770);

    printf("=== A: default ACL only (exactly what vold does) ===\n");
    write_acl(D2, XATTR_ACL_DEFAULT, 0770, (const gid_t[]){AID_EVERYBODY}, 1);
    dump_acl(D2, XATTR_ACL_DEFAULT);
    printf("    uid 1023 creates a file with umask 0077, requested mode 0666:\n");
    AS_MP(ACT_CREATE, D2 "/f_by_mp", 0666);
    stat_line(D2 "/f_by_mp");
    printf("    (mode 0666 => f2fs applied the default ACL and defeated the umask)\n");
    dump_acl(D2 "/f_by_mp", XATTR_ACL_ACCESS);

    printf("\n=== B: can app1 open that file? (inherited access ACL) ===\n");
    AS_APP(ACT_OPEN_RDWR, D2 "/f_by_mp", 0);

    printf("\n=== C: access ACL on the dir itself, set by setxattr after chmod ===\n");
    grant_dir(D2, 0770);
    dump_acl(D2, XATTR_ACL_ACCESS);
    AS_APP(ACT_CREATE, D2 "/f_by_app1", 0666);
    AS_APP(ACT_LIST, D2, 0);

    printf("\n--- sync; echo 3 > /proc/sys/vm/drop_caches ---\n");
    fflush(stdout);
    system("sync; echo 3 > /proc/sys/vm/drop_caches");

    printf("\n=== D: the same operations after the inode cache was dropped ===\n");
    AS_APP(ACT_OPEN_RDWR, D2 "/f_by_mp", 0);
    AS_APP(ACT_LIST, D2, 0);
    AS_APP(ACT_CREATE, D2 "/f_by_app1b", 0666);
    stat_line(D2 "/f_by_app1b");
    dump_acl(D2 "/f_by_app1b", XATTR_ACL_ACCESS);
    AS_APP2(ACT_OPEN_RDWR, D2 "/f_by_app1b", 0);

    system("rm -rf " D2);
    return 0;
}
static int cmd_dbg(void) {
    printf("=== f2fs (same fs as /data/media) ===\n");
    system("rm -rf " S);
    mkdir(S, 0770);
    chown(S, AID_MEDIA_RW, AID_MEDIA_RW);
    chmod(S, 02770);
    grant_dir(S, 0770);
    stat_line(S);
    dump_acl(S, XATTR_ACL_ACCESS);
    run_as(ACT_PROBE, S, 0, APP_UID, APP_GID, APP_GROUPS, 7, "app1");
    run_as(ACT_PROBE, S, 0, MP_UID, MP_GID, MP_GROUPS, 2, "mediaprov");

    printf("\n=== tmpfs control (known-good POSIX ACL support) ===\n");
    system("mkdir -p /mnt/acltest && chmod 755 /mnt/acltest");
    if (system("mount -t tmpfs -o acl,size=4m tmpfs /mnt/acltest") != 0) printf("tmpfs mount failed\n");
    const char *t = "/mnt/acltest/d";
    mkdir(t, 0770);
    chown(t, AID_MEDIA_RW, AID_MEDIA_RW);
    chmod(t, 02770);
    grant_dir(t, 0770);
    stat_line(t);
    dump_acl(t, XATTR_ACL_ACCESS);
    run_as(ACT_PROBE, t, 0, APP_UID, APP_GID, APP_GROUPS, 7, "app1");
    run_as(ACT_PROBE, t, 0, MP_UID, MP_GID, MP_GROUPS, 2, "mediaprov");

    printf("\n=== selinux ===\n      enforce=");
    fflush(stdout);
    system("cat /sys/fs/selinux/enforce");
    system("dmesg | grep -i 'avc:' | tail -12");

    system("umount /mnt/acltest 2>/dev/null; rmdir /mnt/acltest 2>/dev/null");
    system("rm -rf " S);
    return 0;
}

static int cmd_acl(void) {
    system("rm -rf " S);

    printf("-- phase 0: what vold itself wrote on this device --\n");
    stat_line("/data/media");
    dump_acl("/data/media", XATTR_ACL_DEFAULT);
    stat_line("/data/media/0");
    dump_acl("/data/media/0", XATTR_ACL_DEFAULT);
    dump_acl("/data/media/0", XATTR_ACL_ACCESS);
    stat_line("/data/media/0/Android/data/com.mixplorer");
    dump_acl("/data/media/0/Android/data/com.mixplorer", XATTR_ACL_DEFAULT);

    if (mkdir(S, 0770) != 0 && errno != EEXIST) {
        perror("mkdir scratch");
        return 1;
    }
    if (chown(S, AID_MEDIA_RW, AID_MEDIA_RW) != 0) printf("      chown: %s\n", strerror(errno));
    chmod(S, 02770);

    printf("\n-- phase 0b: /data/media itself is 0550 media_rw, so an app cannot even\n");
    printf("            traverse it. grant named group 9997 r-x on the root: --\n");
    AS_APP(ACT_LIST, "/data/media/0", 0);
    write_acl("/data/media", XATTR_ACL_ACCESS, 0550, (const gid_t[]){AID_EVERYBODY}, 1);
    dump_acl("/data/media", XATTR_ACL_ACCESS);
    AS_APP(ACT_LIST, "/data/media/0", 0);
    printf("\n-- scratch dir (mimics a real media dir) --\n");
    stat_line(S);

    printf("\n-- phase 1: stock. no ACL. app1 cannot even write into the dir --\n");
    AS_APP(ACT_CREATE, S "/f_noacl", 0666);
    AS_APP(ACT_MKDIR, S "/d_noacl", 0777);

    printf("\n-- phase 2: access+default ACL on the dir, named group 9997 --\n");
    grant_dir(S, 0770);
    dump_acl(S, XATTR_ACL_ACCESS);
    dump_acl(S, XATTR_ACL_DEFAULT);
    AS_APP(ACT_CREATE, S "/f_acl", 0666);
    stat_line(S "/f_acl");
    dump_acl(S "/f_acl", XATTR_ACL_ACCESS);
    AS_APP2(ACT_OPEN_RDWR, S "/f_acl", 0);
    AS_APP(ACT_MKDIR, S "/d_acl", 0777);
    stat_line(S "/d_acl");
    dump_acl(S "/d_acl", XATTR_ACL_DEFAULT);
    AS_APP(ACT_CREATE, S "/d_acl/f_nested", 0666);
    stat_line(S "/d_acl/f_nested");
    AS_APP2(ACT_OPEN_RDWR, S "/d_acl/f_nested", 0);

    printf("\n-- phase 3: file created by MediaProvider (uid 1023), mode 0660 --\n");
    AS_MP(ACT_CREATE, S "/f_mp660", 0660);
    stat_line(S "/f_mp660");
    AS_APP2(ACT_OPEN_RDWR, S "/f_mp660", 0);

    printf("\n-- phase 4: pre-existing legacy file 0670 gid=1023, no ACL --\n");
    AS_MP(ACT_CREATE, S "/f_legacy", 0670);
    chown(S "/f_legacy", AID_MEDIA_RW, AID_MEDIA_RW);
    chmod(S "/f_legacy", 0670);
    removexattr(S "/f_legacy", XATTR_ACL_ACCESS);
    stat_line(S "/f_legacy");
    AS_APP2(ACT_OPEN_RDWR, S "/f_legacy", 0);
    printf("      ...after normalising the file:\n");
    grant_file(S "/f_legacy", 0660);
    dump_acl(S "/f_legacy", XATTR_ACL_ACCESS);
    AS_APP2(ACT_OPEN_RDWR, S "/f_legacy", 0);

    printf("\n-- phase 5: does a later chmod()/chown() destroy the named entry? --\n");
    printf("      chmod(dir, 0770):\n");
    chmod(S, 0770);
    dump_acl(S, XATTR_ACL_ACCESS);
    printf("      chmod(dir, 02770) + chown(dir, 1023, 1023):\n");
    chmod(S, 02770);
    chown(S, AID_MEDIA_RW, AID_MEDIA_RW);
    dump_acl(S, XATTR_ACL_ACCESS);
    AS_APP(ACT_CREATE, S "/f_after_chmod", 0666);
    stat_line(S "/f_after_chmod");
    AS_APP2(ACT_OPEN_RDWR, S "/f_after_chmod", 0);

    printf("\n-- phase 6: dir owned by another app (Android/data style) --\n");
    if (mkdir(S "/d_other", 0770) == 0) {
        chown(S "/d_other", APP_UID, 10907); /* gid = per-app ext gid, like u0_a398_ext */
        chmod(S "/d_other", 02770);
    }
    stat_line(S "/d_other");
    AS_APP2(ACT_LIST, S "/d_other", 0);
    printf("      ...inherit an access ACL from the parent instead:\n");
    grant_dir(S "/d_other", 0770);
    AS_APP2(ACT_LIST, S "/d_other", 0);
    AS_APP(ACT_CREATE, S "/d_other/f_by_app1", 0666);
    stat_line(S "/d_other/f_by_app1");
    AS_APP2(ACT_OPEN_RDWR, S "/d_other/f_by_app1", 0);

    system("rm -rf " S);
    printf("\nscratch removed\n");
    return 0;
}

static int cmd_bind(void) {
    if (unshare(CLONE_NEWNS) != 0) {
        perror("unshare");
        return 1;
    }
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) printf("mount private: %s\n", strerror(errno));

    const char *tgt = "/mnt/user/0/emulated";
    struct statfs before, after;
    statfs(tgt, &before);
    if (mount("/data/media", tgt, NULL, MS_BIND | MS_REC, NULL) != 0) {
        perror("bind /data/media");
        return 1;
    }
    statfs(tgt, &after);
    printf("      statfs(%s): before=0x%lx after=0x%lx\n", tgt, (unsigned long)before.f_type,
           (unsigned long)after.f_type);
    printf("      fuse=0x65735546 sdcardfs=0x5dca2df5 f2fs=0xf2f52010\n");
    stat_line(tgt);
    stat_line("/mnt/user/0/emulated/0");
    AS_APP(ACT_LIST, "/mnt/user/0/emulated", 0);
    AS_APP(ACT_LIST, "/mnt/user/0/emulated/0", 0);
    AS_APP(ACT_LIST, "/mnt/user/0/emulated/0/Download", 0);
    AS_APP(ACT_CREATE, "/mnt/user/0/emulated/0/Download/.bindtest", 0666);
    return 0;
}

/*
 * Q5: what can sdcardfs do that the raw path cannot?
 *
 * sdcardfs synthesises the *visible* mode and owner on every lookup: a file an
 * app created with 0600 still reads as 0660 to every other app, because the
 * mount's mask/gid decide what callers see, not the lower inode. The raw path
 * has no such synthesis — whatever mode the app asks for lands on disk, and the
 * kernel masks the inherited ACL by that mode (posix_acl_create_masq).
 *
 * This battery performs the same operations as one app uid and then checks what
 * a *different* app uid may do, so a raw tree and an sdcardfs mount of the same
 * tree can be compared operation by operation.
 */
static int cmd_gap(const char *base) {
    char f[512], d[512];
    struct statfs sfs;

    mkdir(base, 0777);
    chmod(base, 0777);
    grant_dir(base, 0777);

    if (statfs(base, &sfs) != 0) {
        printf("statfs(%s) -> %s\n", base, strerror(errno));
        return 1;
    }
    printf("=== %s ===\n", base);
    printf("    fs magic = 0x%lx  (%s)\n", (unsigned long)sfs.f_type,
           sfs.f_type == 0x5dca2df5UL   ? "sdcardfs"
           : sfs.f_type == 0xf2f52010UL ? "f2fs"
                                        : "другая");

    snprintf(f, sizeof(f), "%s/f0600", base);
    unlink(f);
    printf("\n-- 1. app1 создаёт файл с режимом 0600\n");
    AS_APP(ACT_CREATE, f, 0600);
    stat_line(f);
    dump_acl(f, XATTR_ACL_ACCESS);
    AS_APP2(ACT_STAT, f, 0);
    AS_APP2(ACT_OPEN_RDONLY, f, 0);

    snprintf(d, sizeof(d), "%s/d0700", base);
    rmdir(d);
    printf("\n-- 2. app1 создаёт каталог с режимом 0700\n");
    AS_APP(ACT_MKDIR, d, 0700);
    stat_line(d);
    AS_APP2(ACT_STAT, d, 0);
    AS_APP2(ACT_LIST, d, 0);

    snprintf(f, sizeof(f), "%s/f0666", base);
    unlink(f);
    printf("\n-- 3. app1 создаёт файл 0666, затем chmod 0600\n");
    AS_APP(ACT_CREATE, f, 0666);
    stat_line(f);
    AS_APP(ACT_CHMOD, f, 0600);
    stat_line(f);
    dump_acl(f, XATTR_ACL_ACCESS);
    AS_APP2(ACT_OPEN_RDONLY, f, 0);

    snprintf(d, sizeof(d), "%s/d0770", base);
    rmdir(d);
    printf("\n-- 4. app1 создаёт каталог 0770, затем chmod 0700\n");
    AS_APP(ACT_MKDIR, d, 0770);
    stat_line(d);
    AS_APP(ACT_CHMOD, d, 0700);
    stat_line(d);
    dump_acl(d, XATTR_ACL_DEFAULT);
    AS_APP2(ACT_LIST, d, 0);

    snprintf(f, sizeof(f), "%s/fown", base);
    unlink(f);
    printf("\n-- 5. app1 пытается chown свой файл\n");
    AS_APP(ACT_CREATE, f, 0666);
    g_new_uid = APP2_UID;
    g_new_gid = APP2_GID;
    AS_APP(ACT_CHOWN, f, 0);
    g_new_uid = (uid_t)-1;
    g_new_gid = 3003; /* группа, в которой app1 состоит */
    AS_APP(ACT_CHOWN, f, 0);
    stat_line(f);
    AS_APP2(ACT_OPEN_RDWR, f, 0);

    /*
     * 6. Гипотеза для патча: доступ ломает не сам режим, а обнулённая ядром
     *    маска ACL. Если ACL переписать ЗАНОВО уже после создания, именованная
     *    запись перестаёт глушиться маской и доступ возвращается. Ровно это
     *    делает storage-fix — и именно поэтому он лечит дерево, созданное до
     *    него. Здесь проверяется, работает ли это для файла, созданного
     *    приложением только что.
     */
    printf("\n-- 6. повторное применение ACL к уже созданному (как storage-fix)\n");
    snprintf(f, sizeof(f), "%s/f0600", base);
    grant_file(f, 0660);
    stat_line(f);
    dump_acl(f, XATTR_ACL_ACCESS);
    AS_APP2(ACT_OPEN_RDONLY, f, 0);

    snprintf(d, sizeof(d), "%s/d0700", base);
    grant_dir(d, 0770);
    stat_line(d);
    AS_APP2(ACT_LIST, d, 0);

    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s acl|bind|gap <dir>|verify <dir>|list <dir>\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "acl")) return cmd_acl();
    if (!strcmp(argv[1], "bind")) return cmd_bind();
    if (!strcmp(argv[1], "dbg")) return cmd_dbg();
    if (!strcmp(argv[1], "dbg2")) return cmd_dbg2();
    if (!strcmp(argv[1], "dbg3")) return cmd_dbg3();
    if (!strcmp(argv[1], "verify") && argc > 2) return cmd_verify(argv[2]);
    if (!strcmp(argv[1], "gap") && argc > 2) return cmd_gap(argv[2]);
    if (!strcmp(argv[1], "list") && argc > 2) {
        printf("=== app1 (uid %u) opendir %s ===\n", APP_UID, argv[2]);
        AS_APP(ACT_LIST, argv[2], 0);
        printf("=== app2 (uid %u) opendir %s ===\n", APP2_UID, argv[2]);
        AS_APP2(ACT_LIST, argv[2], 0);
        return 0;
    }
    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 2;
}
