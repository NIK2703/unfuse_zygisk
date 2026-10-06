/*
 * hook_libc.h — patch bionic entry points in the app process.
 *
 * Shapes modes into the sdcardfs view (group rw/rwx, other cleared) and adds
 * ACLs to objects entering storage. Call ONLY from postAppSpecialize, else the
 * patch leaks into zygote and corrupts libc for every later process.
 */

#pragma once

#include <stddef.h>

// Installs hooks here; returns count installed, *total = attempts (absent symbols
// are skipped). Repeat calls are no-ops.
int hooks_install(int *total);

// Report like "open=ok open64=alias renameat2=skip ..." — for the self-test.
void hooks_report(char *buf, size_t len);

// Writes the Android release the hooks ran on ("android 17 (sdk 37,
// cinnamonbun)", or a note that the profile was borrowed) and returns how many
// targets the patch covers on that release — the same number hooks_install()
// returns, aliases included — or -1 if the release is not in the table, in which
// case a different tally is not a regression, just an unvalidated release. Only
// meaningful after hooks_install(); a diagnostic, so call it on the
// once-per-boot path, not per launch.
int hooks_release(char *buf, size_t len);

// What the loaded images declare about branch protection, for the boot sample:
// "libc: BTI+PAC; модуль: свойства GNU нет". Returns
//    1  libc declares BTI — the patch's bti jc pad is what keeps that working;
//    0  libc declares nothing, true for Android 12, 12L, 13, 14, 15, 16 and 17
//       today;
//   -1  the module declares BTI without having been built for it, i.e. its pages
//       are guarded while its handlers are not landing pads — the one state worth
//       refusing. Independent of hooks_install(), so it can be called any time.
int hooks_bti_report(char *buf, size_t len);

// Whether a path is on shared storage. Internal detail, exposed for the self-test.
int hooks_path_is_storage(const char *path);
