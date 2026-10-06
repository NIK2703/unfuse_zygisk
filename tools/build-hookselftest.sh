#!/usr/bin/env bash
#
# build-hookselftest.sh — сборка проверки правки входов libc под arm64.
#
# Отдельно от build.sh: это не часть модуля, а инструмент разработчика. Файл
# собирается динамическим (иначе dlsym(RTLD_DEFAULT, "open") не нашёл бы libc) и
# кладётся в out/.
#
# Использование:
#   tools/build-hookselftest.sh              # -> out/hookselftest-arm64
#   API=30 tools/build-hookselftest.sh
#   NDK=/path/to/ndk tools/build-hookselftest.sh
#
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/.." && pwd)"
OUT="${OUT:-$ROOT/out/hookselftest-arm64}"
API="${API:-26}"

find_ndk() {
    if [[ -n "${NDK:-}" && -x "${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" ]]; then
        printf '%s\n' "$NDK"; return 0
    fi
    local c
    for c in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK:-}"; do
        [[ -n "$c" && -x "$c/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" ]] && {
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
        if [[ -x "$r/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" ]]; then
            printf '%s\n' "$r"; return 0
        fi
        while IFS= read -r sub; do
            if [[ -x "$sub/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" ]]; then
                printf '%s\n' "$sub"; return 0
            fi
        done < <(ls -1d "$r"/*/ 2>/dev/null | sed 's:/$::' | sort -V -r)
    done
    return 1
}

NDK_DIR="$(find_ndk)" || { echo "NDK не найден. Укажите: NDK=/path/to/ndk $0" >&2; exit 1; }
TOOLCHAIN="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64"
CXX="$TOOLCHAIN/bin/aarch64-linux-android${API}-clang++"
[[ -x "$CXX" ]] || { echo "нет компилятора $CXX (проверьте API=$API)" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"

echo "==> сборка $OUT"
"$CXX" \
    -std=c++20 \
    -O1 \
    -fPIE -pie \
    -fno-exceptions -fno-rtti \
    -Wall -Wextra -Wno-unused-parameter \
    -I"$ROOT/src" \
    "$HERE/hookselftest.cpp" \
    "$ROOT/src/hook_libc.cpp" \
    "$ROOT/src/func_size.cpp" \
    -o "$OUT" \
    -Wl,--build-id=none \
    -ldl -llog

"$TOOLCHAIN/bin/llvm-strip" --strip-unneeded "$OUT" 2>/dev/null || true
echo " ok $(wc -c < "$OUT") байт"
echo
echo "Запуск на устройстве:"
echo "  adb push $OUT /data/local/tmp/hookselftest"
echo "  adb shell chmod 0755 /data/local/tmp/hookselftest"
echo "  adb shell /data/local/tmp/hookselftest"
