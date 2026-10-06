/*
 * runas — запускает команду с заданными uid / gid / дополнительными группами.
 *
 * Нужен для корректной проверки DAC в sdcardfs: реальный процесс приложения
 * получает от Zygote набор дополнительных групп (ProcessList.computeGidsForProcess),
 * в который ВСЕГДА входит userGid = AID_EVERYBODY (9997). Команда `su <uid>`
 * такие группы не выставляет, поэтому проверка через неё даёт ложные отказы.
 *
 * usage: runas <uid> <gid> <gid,gid,...> <cmd> [args...]
 */

#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: runas <uid> <gid> <gid,gid,...> <cmd> [args...]\n");
        return 2;
    }

    uid_t uid = (uid_t)strtoul(argv[1], NULL, 10);
    gid_t gid = (gid_t)strtoul(argv[2], NULL, 10);

    gid_t groups[64];
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(argv[3], ",", &save); tok != NULL && n < 64;
         tok = strtok_r(NULL, ",", &save)) {
        if (*tok == '\0') continue;
        groups[n++] = (gid_t)strtoul(tok, NULL, 10);
    }

    if (setgroups((size_t)n, groups) != 0) {
        fprintf(stderr, "runas: setgroups(%d): %s\n", n, strerror(errno));
        return 1;
    }
    if (setgid(gid) != 0) {
        fprintf(stderr, "runas: setgid(%u): %s\n", (unsigned)gid, strerror(errno));
        return 1;
    }
    if (setuid(uid) != 0) {
        fprintf(stderr, "runas: setuid(%u): %s\n", (unsigned)uid, strerror(errno));
        return 1;
    }

    execvp(argv[4], &argv[4]);
    fprintf(stderr, "runas: exec %s: %s\n", argv[4], strerror(errno));
    return 1;
}
