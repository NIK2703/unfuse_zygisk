#!/usr/bin/env bash
# test-gnu-props.sh — проверка разбора GNU property note (src/gnu_props.h).
#
# Разбор идёт внутри postAppSpecialize при каждом запуске приложения, то есть
# ошибка в нём — это не строка в логе, а падение всех приложений. Поэтому самотест
# собирается с санитайзерами: чтение за границей сегмента должно ловиться здесь, а
# не на телефоне. Проверяются две вещи, и они разные:
#
#   1. сам разбор — tools/gnu-props-selftest.cpp, на синтетических заметках и на
#      настоящей, выгруженной из объекта с -mbranch-protection=standard;
#   2. проводка вокруг него — tools/host-bti-probe.cpp: поиск образа через
#      dl_iterate_phdr, согласованность кода возврата со строкой, усечённые
#      буферы. Заглушек ей не нужно: логгирования в hook_libc.cpp нет.
#
# Если NDK не найден, пункт с настоящей заметкой пропускается — остальное
# проверяется всё равно. Если у хостового компилятора нет рантаймов санитайзеров
# (MSYS2: libasan/libubsan ставятся отдельно), сборка повторяется без них, о чём
# печатается предупреждение.
#
# Использование: tools/test-gnu-props.sh
#   CXX=g++            другой компилятор хоста
#   SAN=               отключить санитайзеры
#   NDK=/path/to/ndk   явный NDK
set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/.." && pwd)"

SAN="${SAN:--fsanitize=address,undefined -fno-sanitize-recover=all}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Компилятор ХОСТА. Нельзя брать просто `clang++` из PATH: если NDK в PATH, там
# первым окажется его clang++, а он хостовых рантаймов санитайзеров не содержит и
# падает на линковке невнятной ошибкой про libclang_rt.asan. Проверять
# -dumpmachine бесполезно — кросс-компилятор NDK отвечает хостовым тройным именем.
# Поэтому критерий один и прямой: собрать этой командой хостовый бинарник.
#
# Абсолютные пути в списке не для красоты: при NDK в PATH короткое имя `clang++`
# занято, а системный компилятор остаётся доступен только так. На этой машине g++
# вдобавок не линкует вовсе (ld не находит -latomic_asneeded), поэтому одного
# clang++ мало.
pick_cxx() {
    local c
    for c in "${CXX:-}" clang++ /usr/bin/clang++ /usr/local/bin/clang++ \
             g++ /usr/bin/g++ c++ /usr/bin/c++; do
        [[ -n "$c" ]] || continue
        command -v "$c" >/dev/null 2>&1 || continue
        # shellcheck disable=SC2086
        if printf 'int main(){return 0;}\n' |
           "$c" -std=c++20 -x c++ - $SAN -o "$WORK/cxxprobe" >/dev/null 2>&1; then
            printf '%s\n' "$c"; return 0
        fi
    done
    return 1
}

CXX="$(pick_cxx)" || {
    # Рантаймов санитайзеров может не быть вовсе (MSYS2 ставит libasan/libubsan
    # отдельными пакетами, и тогда ld падает на пустом месте). Повторяем без них:
    # тест важнее инструмента, но предупредить надо.
    if [[ -n "$SAN" ]]; then
        SAN=""
        CXX="$(pick_cxx)" && echo "внимание: санитайзеры недоступны, сборка без них" >&2
    fi
    if [[ -z "${CXX:-}" ]]; then
        echo "не нашёл компилятор хоста (пробовал clang++, /usr/bin/clang++, g++, c++)." >&2
        echo "Задайте явно: CXX=/usr/bin/clang++ $0" >&2
        exit 1
    fi
}

# Каталог prebuilt назван по ХОСТУ, а не по цели: жёсткий linux-x86_64 не видит
# исправный NDK на Windows. То же правило, что в build.sh и build-hookselftest.sh.
find_host_tag() {
    local d tag
    for d in "${NDK:-}" "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK:-}"; do
        [[ -n "$d" && -d "$d/toolchains/llvm/prebuilt" ]] || continue
        while IFS= read -r tag; do
            [[ -n "$tag" ]] && { printf '%s\n' "$tag"; return 0; }
        done < <(ls -1 "$d/toolchains/llvm/prebuilt" 2>/dev/null)
    done
    case "$(uname -s 2>/dev/null)" in
        Darwin) printf 'darwin-x86_64\n' ;;
        MINGW*|MSYS*|CYGWIN*) printf 'windows-x86_64\n' ;;
        *)      printf 'linux-x86_64\n' ;;
    esac
}

# MSYS2 печатает пути как /e/foo или /tmp/foo, а нативный clang и g++ их не
# понимают: им нужно E:/foo. cygpath -m делает это правильно; sed — грубый запас
# на случай его отсутствия (он не умеет /tmp, зато умеет буквы дисков).
hostpath() {
    case "$(uname -s 2>/dev/null)" in
        MINGW*|MSYS*|CYGWIN*)
            if command -v cygpath >/dev/null 2>&1; then
                cygpath -m "$1"
            else
                printf '%s\n' "$1" | sed -E 's|^/([a-zA-Z])/|\1:/|'
            fi
            ;;
        *) printf '%s\n' "$1" ;;
    esac
}

find_ndk() {
    local probe="toolchains/llvm/prebuilt/$(find_host_tag)/bin/clang"
    if [[ -n "${NDK:-}" && -x "${NDK}/$probe" ]]; then
        printf '%s\n' "$NDK"; return 0
    fi
    local c
    for c in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK:-}"; do
        [[ -n "$c" && -x "$c/$probe" ]] && {
            printf '%s\n' "$c"; return 0; }
    done
    local roots=(
        "$HOME/projects/tools/android-sdk/ndk"
        "$HOME/Android/Sdk/ndk"
        "$HOME/AppData/Local/Android/Sdk/ndk"
        "$HOME/Library/Android/sdk/ndk"
        /opt/android-sdk/ndk
        /opt/android-ndk
    )
    local r sub
    for r in "${roots[@]}"; do
        [[ -d "$r" ]] || continue
        if [[ -x "$r/$probe" ]]; then
            printf '%s\n' "$r"; return 0
        fi
        while IFS= read -r sub; do
            if [[ -x "$sub/$probe" ]]; then
                printf '%s\n' "$sub"; return 0
            fi
        done < <(ls -1d "$r"/*/ 2>/dev/null | sed 's:/$::' | sort -V -r)
    done
    return 1
}

rc_total=0

echo "==> 1/2 разбор заметки ($CXX ${SAN:-без санитайзеров})"
# shellcheck disable=SC2086
"$CXX" -std=c++20 -O1 -g $SAN -fno-exceptions -fno-rtti \
    -Wall -Wextra -Wno-unused-parameter \
    -I"$(hostpath "$ROOT/src")" \
    "$(hostpath "$HERE/gnu-props-selftest.cpp")" \
    -o "$(hostpath "$WORK/selftest")" || { echo "сборка не удалась" >&2; exit 1; }

NOTE=""
if NDK_DIR="$(find_ndk)"; then
    TC="$NDK_DIR/toolchains/llvm/prebuilt/$(find_host_tag)/bin"
    printf 'int probe(void) { return 1; }\n' > "$WORK/probe.c"
    "$TC/clang" --target=aarch64-linux-android36 -O2 -c \
        -mbranch-protection=standard "$(hostpath "$WORK/probe.c")" \
        -o "$(hostpath "$WORK/probe.o")"
    if "$TC/llvm-objcopy" \
            --dump-section ".note.gnu.property=$(hostpath "$WORK/probe.note")" \
            "$(hostpath "$WORK/probe.o")" 2>/dev/null && [[ -s "$WORK/probe.note" ]]; then
        NOTE="$WORK/probe.note"
    else
        echo "    NDK есть, но секцию выгрузить не удалось — пункт пропущен" >&2
    fi
else
    echo "    NDK не найден — настоящая заметка не проверяется" >&2
fi

echo
if [[ -n "$NOTE" ]]; then
    "$WORK/selftest" "$(hostpath "$NOTE")" || rc_total=1
else
    "$WORK/selftest" || rc_total=1
fi

echo
echo "==> 2/2 проводка hooks_bti_report"
# host-bti-probe.cpp — хостовый ELF-зонд: dl_iterate_phdr, dlfcn.h, sys/mman.h.
# В MinGW их нет вовсе, поэтому на Windows пункт не собирается; на Linux/macOS он
# обязателен, и провал сборки там — провал теста.
if ! printf '#include <dlfcn.h>\n#include <sys/mman.h>\nint main(){return 0;}\n' |
     "$CXX" -std=c++20 -x c++ - -o "$(hostpath "$WORK/posixprobe")" >/dev/null 2>&1; then
    echo "    у хостового компилятора нет POSIX-заголовков (dlfcn.h, sys/mman.h) —" >&2
    echo "    пункт пропущен; он проверяется только на Linux/macOS" >&2
else
    # shellcheck disable=SC2086
    "$CXX" -std=c++20 -O1 -g $SAN -D_GNU_SOURCE -fno-exceptions -fno-rtti \
        -Wall -Wextra -Wno-unused-parameter \
        -I"$(hostpath "$ROOT/src")" \
        "$(hostpath "$HERE/host-bti-probe.cpp")" \
        "$(hostpath "$ROOT/src/hook_libc.cpp")" \
        "$(hostpath "$ROOT/src/func_size.cpp")" \
        -o "$(hostpath "$WORK/host-bti")" -ldl || { echo "сборка не удалась" >&2; exit 1; }

    echo
    "$WORK/host-bti" || rc_total=1
fi

echo
if [[ "$rc_total" == "0" ]]; then
    echo "всё сходится"
else
    echo "есть расхождения" >&2
fi
exit "$rc_total"
