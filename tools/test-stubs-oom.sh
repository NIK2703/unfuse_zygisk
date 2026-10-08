#!/usr/bin/env bash
# test-stubs-oom.sh — prove the Stubs growth path in tools/stubs.h is safe when
# realloc fails, and that it was NOT safe before.
#
# Host-side, unlike the other test-*.sh here (those run on the device). The
# branch under test is the allocator-failure branch; nothing on a device makes a
# 4 KB realloc fail on demand, so the allocator has to be supplied. See the
# header of stubs-oom-test.c for what is being proven.
#
# The A/B is the point of this script. The same probe is built twice:
#
#   СТАРЫЙ — against a copy of tools/stubs.h whose stubs_add() body is put back
#            to the two-realloc form by one exact textual substitution, asserted
#            to match exactly once. Without the assertion a drifted search string
#            would leave the "old" build a copy of the new one, and the A/B would
#            prove nothing;
#   НОВЫЙ  — against tools/stubs.h as it stands.
#
# Expected: СТАРЫЙ reports the double free (rc 1), НОВЫЙ does not (rc 0). If both
# agree, the probe is not reaching the branch and the run is a failure, not a
# pass.
#
# Everything runs with relative paths from a scratch directory under out/,
# because this is used under MSYS2 on Windows: absolute /c/... paths handed to a
# native compiler are the usual way this kind of script breaks on that host.
set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/.." && pwd)"
WORK="$ROOT/out/stubs-oom"
PY="${PY:-python}"

CC="${CC:-}"
if [[ -z "$CC" ]]; then
    for c in cc gcc clang; do
        command -v "$c" >/dev/null 2>&1 && { CC="$c"; break; }
    done
fi
[[ -n "$CC" ]] || { echo "нет C-компилятора на хосте; задайте CC=..." >&2; exit 2; }
command -v "$PY" >/dev/null 2>&1 || { echo "нет $PY; задайте PY=..." >&2; exit 2; }

rm -rf "$WORK"
mkdir -p "$WORK"
cp "$HERE/stubs.h"          "$WORK/stubs.h"
cp "$HERE/stubs-oom-test.c" "$WORK/stubs-oom-test.c"

# The A/B copy, made before the substitution so the "new" build gets a pristine
# file. Run with the scratch directory as cwd and a bare filename: the managed
# Python on Windows does not understand the /c/... paths this shell prints, and a
# relative name needs no conversion on any host.
cp "$HERE/stubs.h" "$WORK/stubs-new.h"
(cd "$WORK" && "$PY" - stubs.h <<'PYEOF'
import sys

path = sys.argv[1]
src = open(path, encoding="utf-8").read()

NEW = """        uint64_t *t = realloc(st->target, cap * sizeof(uint64_t));
        if (!t) return -1;
        st->target = t;
        uint64_t *v = realloc(st->va, cap * sizeof(uint64_t));
        if (!v) return -1;
        st->va = v;
        st->cap = cap;"""

OLD = """        uint64_t *t = realloc(st->target, cap * sizeof(uint64_t));
        uint64_t *v = realloc(st->va, cap * sizeof(uint64_t));
        if (!t || !v) { free(t); free(v); return -1; }
        st->target = t;
        st->va = v;
        st->cap = cap;"""

n = src.count(NEW)
if n != 1:
    sys.exit(f"подстановка: исправленный текст найден {n} раз(а), ожидался 1 — "
             f"stubs_add() разошёлся с тестом, A/B был бы пустым")
open(path, "w", encoding="utf-8", newline="").write(src.replace(NEW, OLD))
print("  подстановка: старое тело восстановлено (1 совпадение)")
PYEOF
) || exit 2

# ---------------------------------------------------------------------- run
#
# Both the exit code AND the marker are checked. The code alone is not enough:
# the probe returns 1 whenever any assertion fails, so a run whose injection
# never fired also returns 1 — for the old build that is the expected code, which
# is how a vacuous A/B slips through. The marker is only produced by case 3, and
# only after its stubs_free(), so it cannot be reached without the injection
# having fired.
run_one() {
    local label="$1" src="$2" want_rc="$3" want_mark="$4"
    echo
    echo "=== $label ==="
    local out rc
    out="$(
        cd "$WORK" || exit 2
        "$CC" -std=c11 -O1 -Wall -Wextra -Wno-unused-function \
              -DVOLD_STUBS_SRC="\"$src\"" \
              stubs-oom-test.c -o "probe-$label" 2>&1 || exit 2
        ./"probe-$label" 2>&1
    )"
    rc=$?
    printf '%s\n' "$out"
    echo "  rc=$rc (ожидается $want_rc)"

    local mark
    mark="$(printf '%s\n' "$out" | sed -n 's/^МЕТКА: //p')"
    echo "  метка=$mark (ожидается $want_mark)"

    local bad=0
    [[ "$rc" == "$want_rc" ]]   || { echo "  РАСХОЖДЕНИЕ: ожидался rc=$want_rc"; bad=1; }
    [[ "$mark" == "$want_mark" ]] || { echo "  РАСХОЖДЕНИЕ: ожидалась метка $want_mark"; bad=1; }
    return "$bad"
}

echo "=== stubs-oom: рост Stubs при отказе realloc (хост, $CC) ==="
bad=0
run_one "старый" "stubs.h"     1 "двойное-освобождение" || bad=1
run_one "новый"  "stubs-new.h" 0 "освобождение-чисто"   || bad=1

echo
if [[ "$bad" == "0" ]]; then
    echo "ИТОГ: ок — старое тело даёт двойное освобождение, новое нет"
else
    echo "ИТОГ: ПРОВАЛ — A/B не разделил старое и новое поведение"
fi
exit "$bad"
