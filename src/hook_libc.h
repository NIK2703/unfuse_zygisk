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

// Whether a path is on shared storage. Internal detail, exposed for the self-test.
int hooks_path_is_storage(const char *path);
