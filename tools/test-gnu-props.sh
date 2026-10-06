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
#      буферы. Ей нужна заглушка android/log.h, поэтому она собирается здесь, а
#      не отдельным скриптом.
#
# Если NDK не найден, пункт с настоящей заметкой пропускается — остальное
# проверяется всё равно.
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
    echo "не нашёл компилятор хоста (пробовал clang++, /usr/bin/clang++, g++, c++" >&2
    echo "с SAN=\"$SAN\"). Задайте явно: CXX=/usr/bin/clang++ $0" >&2
    exit 1
}

find_ndk() {
    if [[ -n "${NDK:-}" && -x "${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ]]; then
        printf '%s\n' "$NDK"; return 0
    fi
    local c
    for c in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK:-}"; do
        [[ -n "$c" && -x "$c/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ]] && {
            printf '%s\n' "$c"; return 0; }
    done
    local roots=(
        "$HOME/projects/tools/android-sdk/ndk"
        "$HOME/Android/Sdk/ndk"
        "$HOME/Library/Android/sdk/ndk"
        /opt/android-sdk/ndk
        /opt/android-ndk
    )
    local r sub
    for r in "${roots[@]}"; do
        [[ -d "$r" ]] || continue
        if [[ -x "$r/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ]]; then
            printf '%s\n' "$r"; return 0
        fi
        while IFS= read -r sub; do
            if [[ -x "$sub/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ]]; then
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
    -I"$ROOT/src" \
    "$HERE/gnu-props-selftest.cpp" \
    -o "$WORK/selftest" || { echo "сборка не удалась" >&2; exit 1; }

NOTE=""
if NDK_DIR="$(find_ndk)"; then
    TC="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin"
    printf 'int probe(void) { return 1; }\n' > "$WORK/probe.c"
    "$TC/clang" --target=aarch64-linux-android36 -O2 -c \
        -mbranch-protection=standard "$WORK/probe.c" -o "$WORK/probe.o"
    if "$TC/llvm-objcopy" --dump-section ".note.gnu.property=$WORK/probe.note" \
            "$WORK/probe.o" 2>/dev/null && [[ -s "$WORK/probe.note" ]]; then
        NOTE="$WORK/probe.note"
    else
        echo "    NDK есть, но секцию выгрузить не удалось — пункт пропущен" >&2
    fi
else
    echo "    NDK не найден — настоящая заметка не проверяется" >&2
fi

echo
if [[ -n "$NOTE" ]]; then
    "$WORK/selftest" "$NOTE" || rc_total=1
else
    "$WORK/selftest" || rc_total=1
fi

echo
echo "==> 2/2 проводка hooks_bti_report (заглушка android/log.h)"
mkdir -p "$WORK/stub/android"
cat > "$WORK/stub/android/log.h" <<'EOF'
#pragma once
enum {
    ANDROID_LOG_UNKNOWN = 0, ANDROID_LOG_DEFAULT = 1, ANDROID_LOG_VERBOSE = 2,
    ANDROID_LOG_DEBUG = 3, ANDROID_LOG_INFO = 4, ANDROID_LOG_WARN = 5,
    ANDROID_LOG_ERROR = 6, ANDROID_LOG_FATAL = 7, ANDROID_LOG_SILENT = 8,
};
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
EOF
cat > "$WORK/stub/logstub.cc" <<'EOF'
#include <stdarg.h>
int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio; (void)tag; (void)fmt; return 0;
}
EOF

# shellcheck disable=SC2086
"$CXX" -std=c++20 -O1 -g $SAN -D_GNU_SOURCE -fno-exceptions -fno-rtti \
    -Wall -Wextra -Wno-unused-parameter \
    -I"$ROOT/src" -I"$WORK/stub" \
    "$HERE/host-bti-probe.cpp" "$ROOT/src/hook_libc.cpp" "$ROOT/src/func_size.cpp" \
    "$WORK/stub/logstub.cc" \
    -o "$WORK/host-bti" -ldl || { echo "сборка не удалась" >&2; exit 1; }

echo
"$WORK/host-bti" || rc_total=1

echo
if [[ "$rc_total" == "0" ]]; then
    echo "всё сходится"
else
    echo "есть расхождения" >&2
fi
exit "$rc_total"
