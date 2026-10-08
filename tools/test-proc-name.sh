#!/usr/bin/env bash
# test-proc-name.sh — build and run the cmdline_is_vold() rule test.
#
# Host-side, and that is the point of the split: tools/proc-name.h depends on
# nothing but <string.h>, so the rule that decides "is this process vold?" can be
# checked against the strings a device actually produces without a device, a root
# shell, or an ELF file. The same rule reached from tools/vold-common.h could only
# be tested on the device — that header includes <elf.h>, which MSYS2 does not
# ship.
#
# The strings the test feeds in were read off the device, not invented; see the
# header of proc-name-test.c.
#
# Relative paths from a scratch directory under out/, because this is used under
# MSYS2 on Windows: absolute /c/... paths handed to a native compiler are the
# usual way this kind of script breaks on that host.
set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/.." && pwd)"
WORK="$ROOT/out/proc-name"

CC="${CC:-}"
if [[ -z "$CC" ]]; then
    for c in cc gcc clang; do
        command -v "$c" >/dev/null 2>&1 && { CC="$c"; break; }
    done
fi
[[ -n "$CC" ]] || { echo "нет C-компилятора на хосте; задайте CC=..." >&2; exit 2; }

rm -rf "$WORK"
mkdir -p "$WORK"
cp "$HERE/proc-name.h"      "$WORK/proc-name.h"
cp "$HERE/proc-name-test.c" "$WORK/proc-name-test.c"

echo "=== proc-name: cmdline_is_vold (хост, $CC) ==="
(
    cd "$WORK" || exit 2
    "$CC" -std=c11 -O1 -Wall -Wextra -Wno-unused-function \
          proc-name-test.c -o proc-name-test || exit 2
    ./proc-name-test
)
rc=$?
echo "rc=$rc (ожидается 0)"
exit "$rc"
