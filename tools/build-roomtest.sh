#!/usr/bin/env bash
# build-roomtest.sh — build the arm64 find_handler_room self-test. Developer
# tool, not part of the module; built dynamic into out/.
# Usage: run it, or set API / NDK.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/.." && pwd)"
OUT="${OUT:-$ROOT/out/roomtest-arm64}"
API="${API:-26}"

# The prebuilt directory is named after the host, so it is discovered the same
# way build.sh does it rather than hardcoded to linux-x86_64.
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
    local tag probe
    tag="$(find_host_tag)"
    probe="toolchains/llvm/prebuilt/$tag/bin/clang"
    local c
    for c in "${NDK:-}" "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK:-}"; do
        [[ -n "$c" && -x "$c/$probe" ]] && { printf '%s\n' "$c"; return 0; }
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
        [[ -x "$r/$probe" ]] && { printf '%s\n' "$r"; return 0; }
        while IFS= read -r sub; do
            [[ -x "$sub/$probe" ]] && { printf '%s\n' "$sub"; return 0; }
        done < <(ls -1d "$r"/*/ 2>/dev/null | sed 's:/$::' | sort -V -r)
    done
    return 1
}

NDK_DIR="$(find_ndk)" || { echo "NDK не найден. Укажите: NDK=/path/to/ndk $0" >&2; exit 1; }
TOOLCHAIN="$NDK_DIR/toolchains/llvm/prebuilt/$(find_host_tag)"
CC="$TOOLCHAIN/bin/aarch64-linux-android${API}-clang"
[[ -x "$CC" ]] || { echo "нет компилятора $CC (проверьте API=$API)" >&2; exit 1; }

# Paths the MSYS2 shell prints (/c/foo) are not understood by the Windows clang
# this same shell invokes: it needs C:/foo.
hostpath() {
    case "$(find_host_tag)" in
        windows-*) printf '%s\n' "$1" | sed -E 's|^/([a-zA-Z])/|\1:/|' ;;
        *)         printf '%s\n' "$1" ;;
    esac
}

mkdir -p "$(dirname "$OUT")"

echo "==> сборка $OUT"
"$CC" \
    -std=c11 \
    -O1 \
    -fPIE -pie \
    -Wall -Wextra -Wno-unused-parameter \
    -Wno-unused-function \
    "$(hostpath "$HERE/roomtest.c")" \
    -o "$(hostpath "$OUT")" \
    -Wl,--build-id=none

"$TOOLCHAIN/bin/llvm-strip" --strip-all "$(hostpath "$OUT")" 2>/dev/null || true
echo " ok $(wc -c < "$OUT") байт"
echo
echo "Запуск на устройстве:"
echo "  adb push $OUT /data/local/tmp/roomtest"
echo "  adb shell chmod 0755 /data/local/tmp/roomtest"
echo "  adb shell /data/local/tmp/roomtest"
