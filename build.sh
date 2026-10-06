#!/usr/bin/env bash
#
# build.sh — сборка Zygisk-модуля sdcardfs-restore.
#
# Использование:
#   ./build.sh                  # arm64-v8a + armeabi-v7a, затем zip в out/
#   ./build.sh arm64-v8a        # только один ABI
#   API=30 ./build.sh           # другой android API level (по умолчанию 26)
#   ZIP=0 ./build.sh            # не паковать zip
#   NDK=/path/to/ndk ./build.sh # явный путь к NDK
#   STRIP=0 ./build.sh          # не стрипать (для отладки)
#
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/src/sdcardfs_restore.cpp"
ZYG_DIR="$HERE/module/zygisk"
OUT_DIR="$HERE/out"

API="${API:-26}"
ZIP="${ZIP:-1}"
STRIP="${STRIP:-1}"
DEBUG="${DEBUG:-0}"

DEFAULT_ABIS=(arm64-v8a armeabi-v7a)

# --------------------------------------------------------------- поиск NDK
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
        # сам каталог уже является NDK?
        if [[ -x "$r/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" ]]; then
            printf '%s\n' "$r"; return 0
        fi
        # каталог-контейнер с версиями: берём самую свежую
        while IFS= read -r sub; do
            if [[ -x "$sub/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" ]]; then
                printf '%s\n' "$sub"; return 0
            fi
        done < <(ls -1d "$r"/*/ 2>/dev/null | sed 's:/$::' | sort -V -r)
    done
    return 1
}

# --------------------------------------------------------------- утилиты
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

# --------------------------------------------------------------- main
NDK_DIR="$(find_ndk)" || die "NDK не найден. Укажите путь: NDK=/path/to/ndk $0"
TOOLCHAIN="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64"
[[ -d "$TOOLCHAIN" ]] || die "нет toolchain: $TOOLCHAIN"

info "NDK:      $NDK_DIR"
info "API:      $API"
info "исходник: $SRC"

[[ -f "$SRC" ]] || die "нет исходника $SRC"
mkdir -p "$ZYG_DIR" "$OUT_DIR"

ABIS=("$@")
[[ ${#ABIS[@]} -eq 0 ]] && ABIS=("${DEFAULT_ABIS[@]}")

# --------------------------------------------------------------- флаги
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
    -llog
)

# --------------------------------------------------------------- сборка
built=()
for abi in "${ABIS[@]}"; do
    prefix="$(triple_for "$abi")" || die "неизвестный ABI: $abi"
    cxx="$TOOLCHAIN/bin/${prefix}${API}-clang++"
    [[ -x "$cxx" ]] || die "нет компилятора $cxx (проверьте API=$API)"

    out="$ZYG_DIR/$abi.so"
    info "сборка $abi -> $(basename "$out")"

    "$cxx" "${COMMON[@]}" "$SRC" -o "$out" "${LDFLAGS[@]}"

    if [[ "$STRIP" == "1" ]]; then
        strip_bin="$TOOLCHAIN/bin/llvm-strip"
        [[ -x "$strip_bin" ]] && "$strip_bin" --strip-unneeded "$out"
    fi

    size="$(wc -c < "$out")"
    ok "$abi: $size байт"

    # Точка входа обязана быть экспортирована и немагленная.
    # zygisk_companion_entry не обязателен: модулю он не нужен (компаньон
    # существует только для чтения конфига из недоступных zygote мест).
    if ! "$TOOLCHAIN/bin/llvm-nm" -D --defined-only "$out" 2>/dev/null | grep -qw zygisk_module_entry; then
        die "в $out отсутствует экспортируемая точка входа zygisk_module_entry"
    fi
    if "$TOOLCHAIN/bin/llvm-nm" -D --defined-only "$out" 2>/dev/null | grep -qw zygisk_companion_entry; then
        ok "$abi: точки входа на месте (module + companion)"
    else
        ok "$abi: точка входа на месте (module)"
    fi

    built+=("$out")
done

# --------------------------------------------------------------- zip
if [[ "$ZIP" == "1" ]]; then
    version="$(sed -n 's/^version=//p' "$HERE/module/module.prop" 2>/dev/null | head -1)"
    [[ -z "$version" ]] && version="dev"
    zipname="sdcardfs_restore-${version}.zip"
    zippath="$OUT_DIR/$zipname"

    # Все shell-скрипты модуля должны быть исполняемыми уже в архиве:
    # customize.sh вызывается установщиком, остальные — загрузчиком модулей.
    chmod 0755 "$HERE/module"/*.sh 2>/dev/null || true

    info "упаковка $zipname"

    # Собираем через python: детерминированно, с сохранением прав, без внешнего
    # zip и без предварительного удаления старого архива.
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
            # Права берём из файловой системы, но .sh всегда исполняемые:
            # иначе при установке модуль может не запустить свои скрипты.
            mode = os.stat(full).st_mode & 0o7777
            if rel.endswith('.sh'):
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
