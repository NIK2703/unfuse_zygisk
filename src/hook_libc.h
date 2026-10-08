/*
 * hook_libc.h — patch bionic entry points in the app process: modes shaped into
 * the sdcardfs view (group rw/rwx, other cleared), ACLs on objects entering
 * storage. ONLY from postAppSpecialize, else the patch leaks into zygote and
 * corrupts libc for every later process.
 */

#pragma once

#include <stddef.h>

// Installed count, *total = attempts; absent symbols skipped, repeats no-ops.
int hooks_install(int *total);

// One token per entry of kHooks[], in order (ok/alias/нет/СБОЙ/переходник/
// коротка/размер?); read by tools/hookselftest.cpp — the only reader there is.
void hooks_report(char *buf, size_t len);

// Writes the release the hooks ran on ("android 17 (sdk 37, cinnamonbun)", or a
// note that the profile was borrowed) and returns how many targets the patch
// covers there, aliases included, or -1 when the release is not in the table,
// where a different tally is no regression. After hooks_install(), per boot.
int hooks_release(char *buf, size_t len);

// Branch protection declared by the loaded images ("libc: BTI+PAC; модуль:
// свойства GNU нет"). Returns 1 = libc declares BTI, which the patch's bti jc
// pad keeps working; 0 = nothing declared, true for Android 11 through 17;
// -1 = the module declares BTI without being built for it — guarded pages,
// handlers not landing pads, the one worth refusing. Callable any time.
int hooks_bti_report(char *buf, size_t len);

// Whether a path is on shared storage. Internal detail, for the self-test.
int hooks_path_is_storage(const char *path);
