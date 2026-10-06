#!/usr/bin/env bash
#
# build.sh — сборка Zygisk-модуля sdcardfs-restore.
#
# Собирает три вещи:
#   module/zygisk/<abi>.so            — сам модуль (C++, NDK, clang++);
#   module/tools/storage-fix-<abi>    — утилита, которая расставляет ACL на сыром
#                                       дереве (C; нужна альтернативному пути,
#                                       когда в ядре нет sdcardfs);
#   module/tools/vold-noacl-<abi>     — патчер vold: делает vold::SetDefaultAcl()
#                                       пустышкой, чтобы vold не перебивал эти
#                                       ACL своей записью для группы 1023 (C).
# Затем пакует module/ в out/sdcardfs_restore-<version>.zip.
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

# abi -> имя бинарника утилиты в module/tools/.
#
# Утилита — нативный исполняемый файл, один на архитектуру (в отличие от
# zygisk/*.so, которые Zygisk сам выбирает по ABI). Поэтому в архиве лежат все
# варианты, а customize.sh на устройстве оставляет нужный и переименовывает его
# в tools/storage-fix.
tool_name_for() {
    case "$1" in
        arm64-v8a)   echo "storage-fix-arm64" ;;
        armeabi-v7a) echo "storage-fix-arm" ;;
        x86_64)      echo "storage-fix-x86_64" ;;
        *)           return 1 ;;
    esac
}

# abi -> имя бинарника патчера vold в module/tools/. По той же причине, что и у
# storage-fix: в архиве лежат все варианты, customize.sh оставляет нужный и
# переименовывает его в tools/vold-noacl.
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
    -ldl
    -llog
)

# Утилита линкуется как обычный бинарник Android (динамически, через bionic):
# она запускается из post-fs-data.sh/service.sh, когда /system уже смонтирован,
# ровно как mount(1) и chcon(1) в тех же скриптах. Статическая сборка дала бы
# 420 КБ вместо 10 КБ и утроила бы вес архива ради ничего.
CFLAGS=(
    -std=c11
    -Oz
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# --------------------------------------------------------------- сборка
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

    # --------------------------------------------- утилита альтернативного пути
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

    # ------------------------------------------------ патчер vold
    #
    # Патчер трогает только память процесса vold, поэтому ABI бинарника не
    # обязан совпадать с ABI vold: он читает и пишет /proc/<pid>/mem, а ELF
    # разбирает сам. Собираем под ту же архитектуру, что и storage-fix, — просто
    # чтобы в архиве не было ничего лишнего.
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
    zipname="sdcardfs_restore-${version}.zip"
    zippath="$OUT_DIR/$zipname"

    # Все shell-скрипты модуля должны быть исполняемыми уже в архиве:
    # customize.sh вызывается установщиком, остальные — загрузчиком модулей.
    # Бинарники утилиты — тоже.
    chmod 0755 "$HERE/module"/*.sh 2>/dev/null || true
    chmod 0755 "$HERE/module"/tools/* 2>/dev/null || true

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
            # Права берём из файловой системы, но .sh и утилиты всегда
            # исполняемые: иначе при установке модуль может не запустить свои
            # скрипты или не найти бинарник storage-fix.
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
