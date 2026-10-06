/*
 * nsprobe — живая проверка механизма варианта B без установки модуля.
 *
 * Повторяет ровно то, что делает preAppSpecialize():
 *   1. unshare(CLONE_NEWNS)            — приватная копия таблицы монтирования
 *   2. make-rprivate на "/"            — чтобы ничего не утекло наружу
 *   3. bind <sdcardfs_src> -> <dst>    — подмена FUSE-ветки на sdcardfs
 *   4. bind /mnt/user/0 -> /storage    — как делает Zygote (MS_BIND|MS_REC)
 *   5. сброс привилегий до uid/gid/groups реального процесса приложения
 *   6. exec команды
 *
 * Пространство имён умирает вместе с процессом, поэтому откат не нужен.
 *
 * usage: nsprobe <sdcardfs_src> <dst> <uid> <gid> <gid,gid,...> <cmd> [args...]
 */

#define _GNU_SOURCE

#include <errno.h>
#include <grp.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef CLONE_NEWNS
#define CLONE_NEWNS 0x00020000
#endif

static void die(const char *what) {
    fprintf(stderr, "nsprobe: %s: %s\n", what, strerror(errno));
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr,
                "usage: nsprobe <sdcardfs_src> <dst> <uid> <gid> <gid,gid,..> <cmd> [args...]\n");
        return 2;
    }

    const char *src = argv[1];
    const char *dst = argv[2];
    uid_t uid = (uid_t)strtoul(argv[3], NULL, 10);
    gid_t gid = (gid_t)strtoul(argv[4], NULL, 10);

    if (unshare(CLONE_NEWNS) != 0) die("unshare(CLONE_NEWNS)");

    /* Ничего не должно утекать в родительский namespace */
    if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) die("make-rprivate /");

    if (mount(src, dst, NULL, MS_BIND | MS_REC, NULL) != 0) die("bind sdcardfs -> dst");

    if (mount("/mnt/user/0", "/storage", NULL, MS_BIND | MS_REC, NULL) != 0)
        die("bind /mnt/user/0 -> /storage");

    gid_t groups[64];
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(argv[5], ",", &save); t != NULL && n < 64;
         t = strtok_r(NULL, ",", &save)) {
        if (*t == '\0') continue;
        groups[n++] = (gid_t)strtoul(t, NULL, 10);
    }

    if (setgroups((size_t)n, groups) != 0) die("setgroups");
    if (setgid(gid) != 0) die("setgid");
    if (setuid(uid) != 0) die("setuid");

    execvp(argv[6], &argv[6]);
    die("execvp");
    return 1;
}
