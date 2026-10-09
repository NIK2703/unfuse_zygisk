#!/usr/bin/env bash
# test-arm32-patch.sh — сверка формы патча входа ARM32 модуля с verifier'ом.
#
# Зачем: на arm32 патч входа бывает трёх видов (Thumb-2 4-выровненный 8 байт,
# Thumb-2 2-выровненный 10 байт, ARM 8 байт), и ошибка в кодировании не падает
# — она молча превращает патч в другую инструкцию (так уже было: `ldr.w pc`
# с перевёрнутым порядком полуслов дизассемблировался как `bl`). Модуль берёт
# байты из src/arm32_patch.h, а источник истины по форме — tools/verify-hook-targets.py,
# проверенный на всех восьми arm32-образах libc (11..17).
#
# Здесь эти двое сталкиваются лбами: C-программа печатает байты по своей
# таблице, питон считает те же байты своей arm_patch_bytes, и любое расхождение
# — провал. Копии констант в тесте нет, поэтому тест не может «сойтись сам с
# собой».
set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/.." && pwd)"
WORK="$ROOT/out/arm32-patch"

CC="${CC:-}"
if [[ -z "$CC" ]]; then
    for c in cc gcc clang; do
        command -v "$c" >/dev/null 2>&1 && { CC="$c"; break; }
    done
fi
[[ -n "$CC" ]] || { echo "нет C-компилятора на хосте; задайте CC=..." >&2; exit 2; }

rm -rf "$WORK"
mkdir -p "$WORK"
cp "$ROOT/src/arm32_patch.h"  "$WORK/arm32_patch.h"
cp "$HERE/test-arm32-patch.c" "$WORK/test-arm32-patch.c"

echo "=== хостовый тест формы патча входа ARM32 ($CC) ==="
(
    cd "$WORK" || exit 2
    "$CC" -std=c11 -O1 -Wall -Wextra -Wno-unused-function \
          test-arm32-patch.c -o probe 2>&1 || exit 2
    ./probe
) > "$WORK/dump.txt"
rc=$?
if (( rc != 0 )); then
    echo "не собрать/запустить C-пробу (rc=$rc)" >&2
    exit 2
fi

python3 - "$ROOT" "$WORK/dump.txt" <<'PY'
import importlib.util
import os
import sys

root, dump = sys.argv[1], sys.argv[2]

spec = importlib.util.spec_from_file_location(
    "verify_hook_targets", os.path.join(root, "tools", "verify-hook-targets.py"))
v = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v)

cases = 0
bad = 0
with open(dump, encoding="utf-8") as fh:
    for line in fh:
        line = line.strip()
        if not line:
            continue
        a, h, w, hexs = line.split()
        raw, handler, width = int(a, 16), int(h, 16), int(w)
        got = bytes.fromhex(hexs)
        exp = v.arm_patch_bytes(raw, handler)
        exp_w = v.arm_patch_size(raw)
        cases += 1
        if got != exp or width != len(exp) or width != exp_w:
            bad += 1
            print("РАСХОДИТСЯ: raw=0x%x handler=0x%x — модуль %s (%d), verifier %s (%d)"
                  % (raw, handler, got.hex(), width, exp.hex(), exp_w))

print("сверено случаев: %d, расхождений: %d" % (cases, bad))
sys.exit(1 if (bad or cases == 0) else 0)
PY
rc=$?

echo
if (( rc == 0 )); then
    echo "ИТОГ: ок (rc=0)"
else
    echo "ИТОГ: ПРОВАЛ (rc=$rc)" >&2
fi
exit "$rc"
