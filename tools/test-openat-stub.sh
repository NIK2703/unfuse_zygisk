#!/usr/bin/env bash
# test-openat-stub.sh — проверка src/openat_stub.h на хосте, без устройства.
#
# Зачем: модуль перестаёт патчить входы open/openat (их переписывает патчер
# порта GCam, и от этого процесс падает) и вместо них патчит единственный стаб
# сисколла __openat. Стаб локальный, dlsym его не видит, поэтому он ищется по
# форме — а «нашлось не то» здесь опаснее «не нашлось»: патч ушёл бы по неверному
# адресу молча.
#
# Что проверяется: см. заголовок tools/test-openat-stub.c. Коротко: на каждом
# эталонном образе стаб находится ровно один раз, найденное смещение совпадает с
# символом __openat из .symtab, размер — с размером символа, и все пути отказа
# (пусто, два стаба, mov без svc, svc без ret, короткий буфер) дают -1.
#
# Тест не может пройти впустую: если finder всегда возвращает -1, падают
# проверки образов; если возвращает мусор — не сходится сверка с символом.
# Дополнительно сверяется число просмотренных образов, чтобы пустой glob не
# выглядел успехом.
#
# Всё считается с относительных путей из рабочего каталога под out/: это MSYS2
# на Windows, и абсолютные /c/... пути, отданные нативному компилятору, — обычный
# способ сломать такой скрипт на этом хосте.
set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/.." && pwd)"
WORK="$ROOT/out/openat-stub"
WANT_IMAGES=16

CC="${CC:-}"
if [[ -z "$CC" ]]; then
    for c in cc gcc clang; do
        command -v "$c" >/dev/null 2>&1 && { CC="$c"; break; }
    done
fi
[[ -n "$CC" ]] || { echo "нет C-компилятора на хосте; задайте CC=..." >&2; exit 2; }

# Оба класса: 8 arm64 (ELF64/AArch64) + 8 arm32 (ELF32/ARM). Форма стаба у них
# разная, и finder'ов тоже два — тест обязан пройти по обоим.
# libc-arm.so назван без суффикса версии (это база, как libc-arm64.so), поэтому
# его приходится перечислять отдельно: под libc-arm-a*.so он не подходит.
#
# Дубли снимаются по содержимому, а не по имени: база и версионная копия — один
# и тот же файл (libc-arm64.so == libc-arm64-a16.so, libc-arm.so ==
# libc-arm-a16.so — md5 совпадает), потому что A16 снимали с устройства дважды,
# под обоими именами. Без этого счёт ниже ловил бы копии вместо пропущенного
# релиза: на 19 файлах в каталоге он и встал (18 под glob против 16 в
# WANT_IMAGES), то есть стаб перестал проверяться на хосте вообще.
# shellcheck disable=SC2207
IMAGES=($(cd "$ROOT/device/libc" && for f in libc-arm64*.so libc-arm-a*.so libc-arm.so; do
    [ -f "$f" ] && printf '%s %s\n' "$(md5sum "$f" | cut -c1-32)" "$f"
done | sort -k2 | awk '!seen[$1]++ { print $2 }'))
if (( ${#IMAGES[@]} != WANT_IMAGES )); then
    echo "эталонных образов ${#IMAGES[@]}, ожидалось $WANT_IMAGES — тест был бы неполным" >&2
    exit 2
fi

rm -rf "$WORK"
mkdir -p "$WORK"
cp "$ROOT/src/openat_stub.h"    "$WORK/openat_stub.h"
cp "$HERE/test-openat-stub.c"   "$WORK/test-openat-stub.c"

echo "=== хостовый тест поиска стаба openat ($CC) ==="
(
    cd "$WORK" || exit 2
    "$CC" -std=c11 -O1 -Wall -Wextra -Wno-unused-function \
          test-openat-stub.c -o probe 2>&1 || exit 2
    ./probe "${IMAGES[@]/#/../../device/libc/}"
) 
rc=$?
echo
if (( rc == 0 )); then
    echo "ИТОГ: ок (rc=0)"
else
    echo "ИТОГ: ПРОВАЛ (rc=$rc)" >&2
fi
exit "$rc"
