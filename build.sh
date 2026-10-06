#!/usr/bin/env bash
#
# build.sh — build the Unfuse Zygisk module.
#
# Builds three things:
#   module/zygisk/<abi>.so          — the module itself (C++, NDK, clang++);
#   module/tools/storage-fix-<abi>  — the tool that places ACLs on the raw tree
#                                     (C; needed by the fallback path when the
#                                     kernel has no sdcardfs);
#   module/tools/vold-noacl-<abi>   — the vold patcher: turns
#                                     vold::SetDefaultAcl() into a no-op so vold
#                                     stops overwriting those ACLs with its own
#                                     group 1023 entry (C).
# Then packs module/ into out/unfuse_zygisk-<version>.zip.
#
# Usage:
#   ./build.sh                  # arm64-v8a + armeabi-v7a, then a zip in out/
#   ./build.sh arm64-v8a        # a single ABI
#   API=30 ./build.sh           # another android API level (default 26)
#   ZIP=0 ./build.sh            # do not pack a zip
#   NDK=/path/to/ndk ./build.sh # explicit NDK path
#   STRIP=0 ./build.sh          # no stripping (debugging)
#
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/src/unfuse_zygisk.cpp"
HOOK_SRC="$HERE/src/hook_libc.cpp"
SIZE_SRC="$HERE/src/func_size.cpp"
TOOLS_SRC="$HERE/tools/storage-fix.c"
NOACL_SRC="$HERE/tools/vold-noacl.c"
ZYG_DIR="$HERE/module/zygisk"
TOOLS_DIR="$HERE/module/tools"
OUT_DIR="$HERE/out"

API="${API:-26}"
ZIP="${ZIP:-1}"
STRIP="${STRIP:-1}"
DEBUG="${DEBUG:-0}"

DEFAULT_ABIS=(arm64-v8a armeabi-v7a)

# --------------------------------------------------------------- NDK lookup
find_ndk() {
    if [[ -n "${NDK:-}" && -x "${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" ]]; then
        printf '%s\n' "$NDK"; return 0
    fi
    local env_candidates=("${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK:-}")
    local c
    for c in "${env_candidates[@]}"; do
        [[ -n "$c" && -x "$c/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" ]] && {
            printf '%s\n' "$c"; return 0; }
    done

    local roots=(
        "$HOME/projects/tools/android-sdk/ndk"
        "$HOME/Android/Sdk/ndk"
        "$HOME/Library/Android/sdk/ndk"
        "/opt/android-sdk/ndk"
        "/opt/android-ndk"
    )
    local r sub
    for r in "${roots[@]}"; do
        [[ -d "$r" ]] || continue
        # is the directory itself an NDK?
        if [[ -x "$r/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" ]]; then
            printf '%s\n' "$r"; return 0
        fi
        # version container directory: take the newest
        while IFS= read -r sub; do
            if [[ -x "$sub/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" ]]; then
                printf '%s\n' "$sub"; return 0
            fi
        done < <(ls -1d "$r"/*/ 2>/dev/null | sed 's:/$::' | sort -V -r)
    done
    return 1
}

# --------------------------------------------------------------- helpers
die()  { printf 'ошибка: %s\n' "$*" >&2; exit 1; }
info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m ok\033[0m %s\n' "$*"; }

# abi -> (triple, clang-prefix)
triple_for() {
    case "$1" in
        arm64-v8a)   echo "aarch64-linux-android" ;;
        armeabi-v7a) echo "armv7a-linux-androideabi" ;;
        x86_64)      echo "x86_64-linux-android" ;;
        *)           return 1 ;;
    esac
}

# abi -> utility binary name in module/tools/.
#
# The utility is a native executable, one per architecture (unlike the
# zygisk/*.so, which Zygisk picks by ABI). So the archive carries every variant
# and customize.sh keeps the right one, renaming it to tools/storage-fix.
tool_name_for() {
    case "$1" in
        arm64-v8a)   echo "storage-fix-arm64" ;;
        armeabi-v7a) echo "storage-fix-arm" ;;
        x86_64)      echo "storage-fix-x86_64" ;;
        *)           return 1 ;;
    esac
}

# abi -> vold patcher binary name in module/tools/. Same reason as storage-fix:
# the archive carries every variant and customize.sh keeps the right one,
# renaming it to tools/vold-noacl.
noacl_name_for() {
    case "$1" in
        arm64-v8a)   echo "vold-noacl-arm64" ;;
        armeabi-v7a) echo "vold-noacl-arm" ;;
        x86_64)      echo "vold-noacl-x86_64" ;;
        *)           return 1 ;;
    esac
}

# --------------------------------------------------------------- main
NDK_DIR="$(find_ndk)" || die "NDK не найден. Укажите путь: NDK=/path/to/ndk $0"
TOOLCHAIN="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64"
[[ -d "$TOOLCHAIN" ]] || die "нет toolchain: $TOOLCHAIN"

info "NDK:      $NDK_DIR"
info "API:      $API"
info "исходник: $SRC"
info "          $HOOK_SRC"
info "          $SIZE_SRC"

[[ -f "$SRC" ]] || die "нет исходника $SRC"
[[ -f "$HOOK_SRC" ]] || die "нет исходника $HOOK_SRC"
[[ -f "$SIZE_SRC" ]] || die "нет исходника $SIZE_SRC"
[[ -f "$TOOLS_SRC" ]] || die "нет исходника $TOOLS_SRC"
[[ -f "$NOACL_SRC" ]] || die "нет исходника $NOACL_SRC"
mkdir -p "$ZYG_DIR" "$TOOLS_DIR" "$OUT_DIR"

ABIS=("$@")
[[ ${#ABIS[@]} -eq 0 ]] && ABIS=("${DEFAULT_ABIS[@]}")

# --------------------------------------------------------------- flags
COMMON=(
    -std=c++20
    -fPIC
    -shared
    -Oz
    -ffunction-sections
    -fdata-sections
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -fno-exceptions
    -fno-rtti
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -Wall
    -Wextra
    -Wno-unused-parameter
    -DNDEBUG
    -I"$HERE/src"
)

if [[ "$DEBUG" == "1" ]]; then
    COMMON=("${COMMON[@]/%-Oz/-O0}")
    COMMON=(-g "${COMMON[@]}")
    STRIP=0
fi

LDFLAGS=(
    -Wl,--gc-sections
    -Wl,--exclude-libs,ALL
    -Wl,-z,max-page-size=16384
    -Wl,-z,noexecstack
    -Wl,-z,relro
    -Wl,-z,now
    -Wl,--build-id=none
    -static-libstdc++
    -ldl
    -llog
)

# The utility links as a normal Android binary (dynamically, via bionic): it runs
# from post-fs-data.sh/service.sh when /system is already mounted, just like
# mount(1) and chcon(1) in those scripts. A static build would give 420 KB
# instead of 10 KB and triple the archive size for nothing.
CFLAGS=(
    -std=c11
    -Oz
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# --------------------------------------------------------------- build
built=()
for abi in "${ABIS[@]}"; do
    prefix="$(triple_for "$abi")" || die "неизвестный ABI: $abi"
    cxx="$TOOLCHAIN/bin/${prefix}${API}-clang++"
    [[ -x "$cxx" ]] || die "нет компилятора $cxx (проверьте API=$API)"

    out="$ZYG_DIR/$abi.so"
    info "сборка $abi -> $(basename "$out")"

    "$cxx" "${COMMON[@]}" "$SRC" "$HOOK_SRC" "$SIZE_SRC" -o "$out" "${LDFLAGS[@]}"

    if [[ "$STRIP" == "1" ]]; then
        strip_bin="$TOOLCHAIN/bin/llvm-strip"
        [[ -x "$strip_bin" ]] && "$strip_bin" --strip-unneeded "$out"
    fi

    size="$(wc -c < "$out")"
    ok "$abi: $size байт"

    # The entry point must be exported and unmangled. zygisk_companion_entry is
    # optional: the module does not need it (a companion exists only for reading
    # config from places zygote cannot reach).
    if ! "$TOOLCHAIN/bin/llvm-nm" -D --defined-only "$out" 2>/dev/null | grep -qw zygisk_module_entry; then
        die "в $out отсутствует экспортируемая точка входа zygisk_module_entry"
    fi
    if "$TOOLCHAIN/bin/llvm-nm" -D --defined-only "$out" 2>/dev/null | grep -qw zygisk_companion_entry; then
        ok "$abi: точки входа на месте (module + companion)"
    else
        ok "$abi: точка входа на месте (module)"
    fi

    built+=("$out")

    # --------------------------------------------- fallback-path utility
    cc="$TOOLCHAIN/bin/${prefix}${API}-clang"
    [[ -x "$cc" ]] || die "нет компилятора $cc (проверьте API=$API)"

    tool="$(tool_name_for "$abi")" || die "нет имени утилиты для $abi"
    tout="$TOOLS_DIR/$tool"
    info "сборка $abi -> $(basename "$tout")"

    "$cc" "${CFLAGS[@]}" "$TOOLS_SRC" -o "$tout" \
        -Wl,--gc-sections -Wl,--build-id=none

    if [[ "$STRIP" == "1" ]]; then
        strip_bin="$TOOLCHAIN/bin/llvm-strip"
        [[ -x "$strip_bin" ]] && "$strip_bin" --strip-all "$tout"
    fi

    ok "$abi: $(basename "$tout") — $(wc -c < "$tout") байт"
    built+=("$tout")

    # ------------------------------------------------ vold patcher
    #
    # The patcher touches only vold's process memory, so its ABI need not match
    # vold's: it reads and writes /proc/<pid>/mem and parses ELF itself. Built
    # for the same architecture as storage-fix, simply so the archive holds
    # nothing extra.
    ntool="$(noacl_name_for "$abi")" || die "нет имени патчера для $abi"
    nout="$TOOLS_DIR/$ntool"
    info "сборка $abi -> $(basename "$nout")"

    "$cc" "${CFLAGS[@]}" "$NOACL_SRC" -o "$nout" \
        -Wl,--gc-sections -Wl,--build-id=none

    if [[ "$STRIP" == "1" ]]; then
        strip_bin="$TOOLCHAIN/bin/llvm-strip"
        [[ -x "$strip_bin" ]] && "$strip_bin" --strip-all "$nout"
    fi

    ok "$abi: $(basename "$nout") — $(wc -c < "$nout") байт"
    built+=("$nout")
done

# --------------------------------------------------------------- zip
if [[ "$ZIP" == "1" ]]; then
    version="$(sed -n 's/^version=//p' "$HERE/module/module.prop" 2>/dev/null | head -1)"
    [[ -z "$version" ]] && version="dev"
    zipname="unfuse_zygisk-${version}.zip"
    zippath="$OUT_DIR/$zipname"

    # Every module shell script must already be executable in the archive:
    # customize.sh is invoked by the installer, the rest by the module loader.
    # Same for the utility binaries.
    chmod 0755 "$HERE/module"/*.sh 2>/dev/null || true
    chmod 0755 "$HERE/module"/tools/* 2>/dev/null || true

    info "упаковка $zipname"

    # Packed through python: deterministic, permissions preserved, no external
    # zip and no need to delete an old archive first.
    python3 - "$HERE/module" "$zippath" <<'PYEOF'
import os, sys, zipfile

root, out = sys.argv[1], sys.argv[2]
tmp = out + ".tmp"
skipped = []

with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for base, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if not d.startswith('.'))
        for f in sorted(files):
            full = os.path.join(base, f)
            rel = os.path.relpath(full, root)
            if f.startswith('.') or rel.endswith('.zip') or rel.endswith('.tmp'):
                skipped.append(rel)
                continue
            zi = zipfile.ZipInfo(rel, date_time=(2026, 1, 1, 0, 0, 0))
            # Permissions come from the filesystem, but .sh files and the tools
            # are always executable: otherwise the module may fail to run its
            # scripts or to find the storage-fix binary on install.
            mode = os.stat(full).st_mode & 0o7777
            if rel.endswith('.sh') or rel.startswith('tools/'):
                mode |= 0o111
            zi.external_attr = mode << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            with open(full, 'rb') as fh:
                z.writestr(zi, fh.read())

os.replace(tmp, out)
if skipped:
    print("  пропущено:", ", ".join(skipped))
PYEOF

    [[ -f "$zippath" ]] || die "не удалось создать $zippath"
    zsize="$(wc -c < "$zippath")"
    ok "готово: $zippath ($zsize байт)"
fi

printf '\n'
info "собрано файлов: ${#built[@]}"
for f in "${built[@]}"; do printf '    %s\n' "$f"; done
