/*
 * proc-name.h — deciding whether a process is the one we mean to patch.
 *
 * Both vold patchers identify vold by name and by nothing else: pid alone is not
 * a handle (vold restarts, and vold.rc carries reboot_on_failure, so patching the
 * wrong process is not a mistake that stays local). The name comes from
 * /proc/<pid>/exe, and — when that cannot be read — from /proc/<pid>/cmdline.
 *
 * ============================== why this is its own file
 *
 * The cmdline rule is a pure string decision, so it can be tested on the host
 * against the strings a device actually produces, without a device, a root
 * shell, or an ELF file. It lives here rather than in vold-common.h for that
 * reason: vold-common.h includes <elf.h>, which MSYS2 does not ship, so anything
 * defined there can only be tested on the device. Same split, same reason as
 * stubs.h.
 *
 * ============================== the two shapes
 *
 *   /proc/<pid>/exe    → the path of the binary, e.g. "/system/bin/vold".
 *                        readlink() fails for another user's process, so this is
 *                        the root path and the only one that is always taken.
 *   /proc/<pid>/cmdline → argv with NUL separators, e.g.
 *                        "/system/bin/vold\0--blkid_context=u:r:blkid:s0\0…".
 *                        World-readable, so this is the non-root path.
 */

#pragma once

#include <stdbool.h>
#include <string.h>

/* The last path component: "/system/bin/vold" -> "vold". */
static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Is this /proc/<pid>/cmdline the vold binary?
 *
 * `cmdline` is the raw contents with every NUL separator rewritten to a space,
 * which is how pid_is_vold() hands it over, and is mutated here (the name is cut
 * out of it). Returns true only for the vold binary itself.
 *
 * Two things have to be right at once, and the obvious simplifications get one
 * of them wrong:
 *
 *  - vold is NOT started bare. Measured on the device:
 *        /system/bin/vold\0--blkid_context=u:r:blkid:s0\0
 *                        \0--blkid_untrusted_context=u:r:blkid_untrusted:s0\0
 *                        \0--fsck_context=u:r:fsck:s0\0
 *                        \0--fsck_untrusted_context=u:r:fsck_untrusted:s0\0
 *    so the string here is "vold --blkid_context=…" and neither strcmp() nor
 *    base_name() alone yields a name from it. Cutting at the first space does.
 *
 *  - vold EXECs a sibling whose name starts the same way. /system/bin/
 *    vold_prepare_subdirs exists on the device and is forked off by vold, so
 *    comparing a 4-byte prefix — which is what makes the glued-arguments string
 *    match — accepts it too, and the tool would patch that child's .plt instead
 *    of vold's. Comparing the whole first word does not.
 */
static bool cmdline_is_vold(char *cmdline) {
    char *p = cmdline;
    while (*p == ' ') p++;
    char *sp = strchr(p, ' ');
    if (sp) *sp = '\0';
    return strcmp(base_name(p), "vold") == 0;
}
