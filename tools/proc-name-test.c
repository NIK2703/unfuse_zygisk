/*
 * proc-name-test.c — cmdline_is_vold() against the strings a device produces.
 *
 * The inputs are not invented: they are what /proc/<pid>/cmdline actually held
 * on the device (Android 16, marble), read by tools/proc-names-dump.sh, with the
 * NUL separators rewritten to spaces exactly as pid_is_vold() does before it
 * calls the rule:
 *
 *   vold pid 892, /proc/892/cmdline:
 *     /system/bin/vold\0--blkid_context=u:r:blkid:s0\0
 *                     \0--blkid_untrusted_context=u:r:blkid_untrusted:s0\0
 *                     \0--fsck_context=u:r:fsck:s0\0
 *                     \0--fsck_untrusted_context=u:r:fsck_untrusted:s0\0
 *
 * and /system/bin/vold_prepare_subdirs was confirmed present on that device — it
 * is the sibling vold execs, and the one the old 4-byte prefix rule accepted.
 *
 * Both rules are evaluated here, not just the current one, so the file states
 * what changed and why rather than only asserting today's behaviour. The old one
 * is reproduced inline: it is three lines, and its whole defect is that it is
 * short enough to look right.
 *
 * Runs on the host (no device, no root): proc-name.h depends on nothing but
 * <string.h>.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "proc-name.h"

/* The rule as it stood before: compare the first four bytes of base_name().
 * Kept here as the thing under contrast — see the header. */
static bool old_cmdline_is_vold(char *cmdline) {
    char *p = cmdline;
    while (*p == ' ') p++;
    return strncmp(base_name(p), "vold", 4) == 0;
}

static int g_fail;

/* cmdline_is_vold() mutates its argument, so every case works on a copy. */
static void check(const char *what, const char *cmdline,
                  bool want_new, bool want_old) {
    char a[512], b[512];
    snprintf(a, sizeof a, "%s", cmdline);
    snprintf(b, sizeof b, "%s", cmdline);

    bool got_new = cmdline_is_vold(a);
    bool got_old = old_cmdline_is_vold(b);

    printf("%-58s новый=%-3s старый=%-3s\n", what,
           got_new ? "да" : "нет", got_old ? "да" : "нет");

    if (got_new != want_new) {
        printf("  ПРОВАЛ: новый ожидался %s\n", want_new ? "да" : "нет");
        g_fail = 1;
    }
    if (got_old != want_old) {
        printf("  ПРОВАЛ: старый ожидался %s\n", want_old ? "да" : "нет");
        g_fail = 1;
    }
}

int main(void) {
    printf("=== proc-name: cmdline_is_vold ===\n");

    /* The real thing, arguments and all — the case the old rule matched only
     * because it stopped after four bytes. */
    check("vold с аргументами (как на устройстве)",
          "/system/bin/vold --blkid_context=u:r:blkid:s0 "
          "--blkid_untrusted_context=u:r:blkid_untrusted:s0 "
          "--fsck_context=u:r:fsck:s0 "
          "--fsck_untrusted_context=u:r:fsck_untrusted:s0",
          true, true);

    check("vold без аргументов", "vold", true, true);
    check("vold полным путём", "/system/bin/vold", true, true);

    /* The false positive: same prefix, different binary. */
    check("vold_prepare_subdirs (сосед, который exec-ает vold)",
          "/system/bin/vold_prepare_subdirs --blkid_context=u:r:blkid:s0",
          false, true);
    check("vold_prepare_subdirs без аргументов",
          "/system/bin/vold_prepare_subdirs", false, true);

    /* Neighbours that must not be accepted either way. */
    check("vold_init (посторонний)", "/system/bin/vold_init", false, true);
    check("vold2", "vold2", false, true);
    check("совсем другой процесс", "/system/bin/netd", false, false);

    /* Shapes the parse has to survive rather than crash on. */
    check("пустая строка", "", false, false);
    check("только пробелы", "   ", false, false);

    /* Leading spaces are skipped, so an EMPTY argv[0] followed by the argument
     * "vold" is accepted. That laxness predates this change and is left alone: it
     * is only reachable if a process is exec'd with an empty argv[0], which no
     * init service — vold included — does, and narrowing it is a behaviour change
     * with no failure it would prevent. Recorded here so the next reader does not
     * have to rediscover it from the " vold" row. */
    check("пробел первым (пустой argv[0], известная слабость)", " vold", true, true);

    check("аргумент со слэшем до имени",
          "vold /data/media/0/vold", true, true);
    check("имя с путём и слэшем в аргументе",
          "/system/bin/vold /x/vold_prepare_subdirs", true, true);

    printf("%s\n", g_fail ? "ИТОГ: ПРОВАЛ" : "ИТОГ: ок");
    return g_fail ? 1 : 0;
}
