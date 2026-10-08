#!/usr/bin/env bash
# build-hookselftest.sh — build the arm64 libc-hook self-test. Developer tool, not
# part of the module; built dynamic into out/. Usage: run it, or set API / NDK.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/.." && pwd)"
OUT="${OUT:-$ROOT/out/hookselftest-arm64}"
API="${API:-26}"

# The prebuilt directory is named after the *host*: linux-x86_64, darwin-x86_64
# or windows-x86_64. Hardcoding linux-x86_64 makes the script refuse to see a
# perfectly good NDK on Windows, so the tag is computed once here — same rule as
# build.sh, which has to survive the same thing.
find_host_tag() {
    local d
    for d in "${NDK:-}" "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK:-}"; do
        [[ -n "$d" && -d "$d/toolchains/llvm/prebuilt" ]] || continue
        local tag
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

find_ndk() {
    local host_tag
    host_tag="$(find_host_tag)"
    local probe="toolchains/llvm/prebuilt/$host_tag/bin/clang++"

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

NDK_DIR="$(find_ndk)" || { echo "NDK не найден. Укажите: NDK=/path/to/ndk $0" >&2; exit 1; }
TOOLCHAIN="$NDK_DIR/toolchains/llvm/prebuilt/$(find_host_tag)"
CXX="$TOOLCHAIN/bin/aarch64-linux-android${API}-clang++"
[[ -x "$CXX" ]] || { echo "нет компилятора $CXX (проверьте API=$API)" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"

# Paths the MSYS2 shell prints (/c/foo) are not understood by the Windows clang
# this same shell invokes: it needs C:/foo. Under a POSIX host the identity
# function is right. Same reason and same rule as build.sh.
hostpath() {
    case "$(find_host_tag)" in
        windows-*) printf '%s\n' "$1" | sed -E 's|^/([a-zA-Z])/|\1:/|' ;;
        *)         printf '%s\n' "$1" ;;
    esac
}

echo "==> сборка $OUT"
"$CXX" \
    -std=c++20 \
    -O1 \
    -fPIE -pie \
    -fno-exceptions -fno-rtti \
    -Wall -Wextra -Wno-unused-parameter \
    -I"$(hostpath "$ROOT/src")" \
    "$(hostpath "$HERE/hookselftest.cpp")" \
    "$(hostpath "$ROOT/src/hook_libc.cpp")" \
    "$(hostpath "$ROOT/src/func_size.cpp")" \
    -o "$(hostpath "$OUT")" \
    -Wl,--build-id=none \
    -ldl -llog

"$TOOLCHAIN/bin/llvm-strip" --strip-unneeded "$OUT" 2>/dev/null || true
echo " ok $(wc -c < "$OUT") байт"
echo
echo "Запуск на устройстве:"
echo "  adb push $OUT /data/local/tmp/hookselftest"
echo "  adb shell chmod 0755 /data/local/tmp/hookselftest"
echo "  adb shell /data/local/tmp/hookselftest"
