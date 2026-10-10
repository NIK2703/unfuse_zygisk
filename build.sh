#!/usr/bin/env bash
#
# build.sh — сборка обоих модулей проекта из одного дерева.
#
#   module/common/ + module/unfuse/           -> out/unfuse-<version>.zip
#   module/common/ + module/unfuse-sdcardfs/  -> out/unfuse-sdcardfs-<version>.zip
#
# Что собирается у каждого варианта:
#
#   unfuse (Zygisk, C++, NDK clang++):
#     module/unfuse/zygisk/<abi>.so   — сам модуль: vold-fusefs заменяет FUSE-маунт
#                                       vold на бинд сырого /data/media, а libc-хуки
#                                       формулируют режимы под это дерево;
#     module/unfuse/tools/storage-fix-<abi>  — ACL группы 9997 на сыром дереве;
#     module/unfuse/tools/vold-noacl-<abi>   — vold::SetDefaultAcl() -> no-op;
#     module/unfuse/tools/vold-fusefs-<abi>  — перехват mount()/umount2() в vold.
#
#   unfuse-sdcardfs (Zygisk, C++, NDK clang++):
#     module/unfuse-sdcardfs/zygisk/<abi>.so — сам модуль: биндит sdcardfs
#                                       /mnt/runtime/*/emulated в namespace приложения.
#     Утилит нет: всё, что нужно, делает storage.sh.
#
# module/common/ — то, что в обоих архивах совпадает: META-INF установщика и lib.sh
# с общими примитивами (смена свойства, перемаркировка /data/media, шаг установщика).
# Перед упаковкой common и каталог варианта складываются в один staging; в архив
# идут ровно они, а не дерево репозитория.
#
# Usage:
#   ./build.sh                      # оба модуля, arm64-v8a + armeabi-v7a
#   ./build.sh unfuse               # только unfuse
#   ./build.sh unfuse-sdcardfs      # только unfuse-sdcardfs
#   ./build.sh unfuse arm64-v8a     # вариант и ABI — в любом порядке
#   API=30 ./build.sh               # другой android API level (default 26)
#   ZIP=0 ./build.sh                # без упаковки
#   NDK=/path/to/ndk ./build.sh     # явный путь к NDK
#   STRIP=0 ./build.sh              # без strip (отладка)
#
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$HERE/module"
OUT_DIR="$HERE/out"

API="${API:-26}"
ZIP="${ZIP:-1}"
STRIP="${STRIP:-1}"
DEBUG="${DEBUG:-0}"

DEFAULT_ABIS=(arm64-v8a armeabi-v7a)
ALL_VARIANTS=(unfuse unfuse-sdcardfs)

# --------------------------------------------------------------- NDK lookup
#
# The prebuilt directory is named after the *host*: linux-x86_64, darwin-x86_64
# or windows-x86_64. Hardcoding linux-x86_64 makes the script refuse to see a
# perfectly good NDK on Windows, so the host tag is computed once here. Both the
# clang++ probe and the toolchain root below use it.
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
    local env_candidates=("${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK:-}")
    local c
    for c in "${env_candidates[@]}"; do
        [[ -n "$c" && -x "$c/$probe" ]] && {
            printf '%s\n' "$c"; return 0; }
    done

    local roots=(
        "$HOME/projects/tools/android-sdk/ndk"
        "$HOME/Android/Sdk/ndk"
        "$HOME/AppData/Local/Android/Sdk/ndk"
        "$HOME/Library/Android/sdk/ndk"
        "/opt/android-sdk/ndk"
        "/opt/android-ndk"
    )
    local r sub
    for r in "${roots[@]}"; do
        [[ -d "$r" ]] || continue
        # is the directory itself an NDK?
        if [[ -x "$r/$probe" ]]; then
            printf '%s\n' "$r"; return 0
        fi
        # version container directory: take the newest, skipping empty ones
        while IFS= read -r sub; do
            if [[ -x "$sub/$probe" ]]; then
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

# Paths the MSYS2 shell prints (/c/foo) are not understood by the Windows clang
# that this same shell invokes: it needs C:/foo. Under a POSIX host the identity
# function is right, so the translation is decided once, from the host tag.
HOST_TAG="$(find_host_tag)"
hostpath() {
    case "$HOST_TAG" in
        windows-*)
            # /c/Users/x -> C:/Users/x ; /h/projects -> H:/projects
            printf '%s\n' "$1" | sed -E 's|^/([a-zA-Z])/|\1:/|'
            ;;
        *) printf '%s\n' "$1" ;;
    esac
}

# abi -> (triple, clang-prefix)
triple_for() {
    case "$1" in
        arm64-v8a)   echo "aarch64-linux-android" ;;
        armeabi-v7a) echo "armv7a-linux-androideabi" ;;
        *)           return 1 ;;
    esac
}

# abi -> суффикс имени утилиты в module/<variant>/tools/.
#
# Утилита — нативный исполняемый файл, по одному на архитектуру (в отличие от
# zygisk/*.so, который Zygisk выбирает по ABI). Поэтому в архив кладутся оба
# варианта, а customize.sh оставляет нужный и переименовывает в tools/<имя>.
abi_suffix_for() {
    case "$1" in
        arm64-v8a)   echo "arm64" ;;
        armeabi-v7a) echo "arm" ;;
        *)           return 1 ;;
    esac
}

# --------------------------------------------------------------- аргументы
#
# Аргументы — имена вариантов и/или ABI в любом порядке. Не названо ни одного
# варианта — собираются оба; ни одного ABI — оба ABI.
VARIANTS=()
ABIS=()
for arg in "$@"; do
    case "$arg" in
        unfuse|unfuse-sdcardfs) VARIANTS+=("$arg") ;;
        arm64-v8a|armeabi-v7a)  ABIS+=("$arg") ;;
        *) die "неизвестный аргумент: $arg (варианты: ${ALL_VARIANTS[*]}; ABI: ${DEFAULT_ABIS[*]})" ;;
    esac
done
[[ ${#VARIANTS[@]} -eq 0 ]] && VARIANTS=("${ALL_VARIANTS[@]}")
[[ ${#ABIS[@]} -eq 0 ]] && ABIS=("${DEFAULT_ABIS[@]}")

# --------------------------------------------------------------- main
NDK_DIR="$(find_ndk)" || die "NDK не найден. Укажите путь: NDK=/path/to/ndk $0"
TOOLCHAIN="$NDK_DIR/toolchains/llvm/prebuilt/$(find_host_tag)"
[[ -d "$TOOLCHAIN" ]] || die "нет toolchain: $TOOLCHAIN"

info "NDK:      $NDK_DIR"
info "API:      $API"
info "варианты: ${VARIANTS[*]}"
info "ABI:      ${ABIS[*]}"

mkdir -p "$OUT_DIR"

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
)

# The utilities link as normal Android binaries (dynamically, via bionic): they
# run from post-fs-data.sh/service.sh when /system is already mounted, just like
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
    # No -I: the two patchers include tools/vold-common.h, and a quoted include
    # resolves next to the including file. android_ver.h is not included here at
    # all — only src/hook_libc.cpp uses it, and the module's own CXXFLAGS carry
    # the -I for it.
)

strip_unneeded() {
    [[ "$STRIP" == "1" ]] || return 0
    local strip_bin="$TOOLCHAIN/bin/llvm-strip"
    [[ -x "$strip_bin" ]] && "$strip_bin" --strip-unneeded "$(hostpath "$1")"
    return 0
}

strip_all() {
    [[ "$STRIP" == "1" ]] || return 0
    local strip_bin="$TOOLCHAIN/bin/llvm-strip"
    [[ -x "$strip_bin" ]] && "$strip_bin" --strip-all "$(hostpath "$1")"
    return 0
}

# --------------------------------------------------------------- упаковка
#
# staging = module/common/ поверх module/<variant>/. Права нормализованы, а не
# взяты с файловой системы: иначе архив зависел бы от umask сборочной машины.
# Скрипты, утилиты и заглушка установщика исполняемые (их читает либо установщик,
# либо загрузчик модуля), остальное 0644. customize.sh выставляет те же права ещё
# раз при установке — здесь важно только то, что сам zip корректен.
pack() {
    local variant="$1"
    local vdir="$MODULE_DIR/$variant"
    local version zipname zippath staging

    version="$(sed -n 's/^version=//p' "$vdir/module.prop" 2>/dev/null | head -1)"
    [[ -z "$version" ]] && version="dev"
    zipname="$variant-${version}.zip"
    zippath="$OUT_DIR/$zipname"
    staging="$OUT_DIR/.staging-$variant"

    chmod 0755 "$vdir"/*.sh 2>/dev/null || true
    chmod 0755 "$vdir"/tools/* 2>/dev/null || true

    rm -rf "$staging"
    mkdir -p "$staging"
    cp -a "$MODULE_DIR/common/." "$staging/"
    cp -a "$vdir/." "$staging/"

    info "упаковка $zipname"

    # Packed through python: deterministic, permissions normalised, no external
    # zip and no need to delete an old archive first. Paths are translated
    # because python here is a native Windows build, not an MSYS one.
    python3 - "$(hostpath "$staging")" "$(hostpath "$zippath")" <<'PYEOF'
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
            # os.path.relpath uses "\" on Windows while zip entries always use
            # "/", so the directory test has to run on the normalized form:
            # otherwise the tools silently lose their executable bit here.
            relposix = rel.replace(os.sep, '/')
            if f.startswith('.') or rel.endswith('.zip') or rel.endswith('.tmp'):
                skipped.append(rel)
                continue
            executable = (rel.endswith('.sh') or f == 'update-binary'
                          or relposix.startswith('tools/'))
            mode = 0o755 if executable else 0o644
            zi = zipfile.ZipInfo(rel, date_time=(2026, 1, 1, 0, 0, 0))
            zi.external_attr = mode << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            with open(full, 'rb') as fh:
                z.writestr(zi, fh.read())

os.replace(tmp, out)
if skipped:
    print("  пропущено:", ", ".join(skipped))
PYEOF

    rm -rf "$staging"
    [[ -f "$zippath" ]] || die "не удалось создать $zippath"
    ok "готово: $zippath ($(wc -c < "$zippath") байт)"
}

# --------------------------------------------------------------- build
built=()
for variant in "${VARIANTS[@]}"; do
    vdir="$MODULE_DIR/$variant"
    [[ -d "$vdir" ]] || die "нет каталога варианта: $vdir"

    # Что у варианта своё: исходники плеча, его линковка и набор утилит.
    cxx_srcs=()
    ld_extra=()
    ctools=()
    case "$variant" in
        unfuse)
            cxx_srcs=("$HERE/src/unfuse_zygisk.cpp" "$HERE/src/hook_libc.cpp" "$HERE/src/func_size.cpp")
            ctools=(storage-fix vold-noacl vold-fusefs)
            ;;
        unfuse-sdcardfs)
            cxx_srcs=("$HERE/src/unfuse_sdcardfs.cpp")
            ld_extra=(-llog)
            ;;
    esac

    for s in "${cxx_srcs[@]}"; do [[ -f "$s" ]] || die "нет исходника $s"; done
    for t in "${ctools[@]}"; do [[ -f "$HERE/tools/$t.c" ]] || die "нет исходника $HERE/tools/$t.c"; done

    info "=== вариант $variant ==="

    for abi in "${ABIS[@]}"; do
        prefix="$(triple_for "$abi")" || die "неизвестный ABI: $abi"
        cxx="$TOOLCHAIN/bin/${prefix}${API}-clang++"
        [[ -x "$cxx" ]] || die "нет компилятора $cxx (проверьте API=$API)"

        mkdir -p "$vdir/zygisk"

        # ---------------------------------------------------- Zygisk-плечо
        out="$vdir/zygisk/$abi.so"
        info "сборка $abi -> $(basename "$out")"

        src_args=()
        for s in "${cxx_srcs[@]}"; do src_args+=("$(hostpath "$s")"); done

        "$cxx" "${COMMON[@]}" "${src_args[@]}" -o "$(hostpath "$out")" \
            "${LDFLAGS[@]}" "${ld_extra[@]}"
        strip_unneeded "$out"

        ok "$abi: $(wc -c < "$out") байт"

        # The entry point must be exported and unmangled. zygisk_companion_entry
        # is optional: the module does not need it (a companion exists only for
        # reading config from places zygote cannot reach).
        if ! "$TOOLCHAIN/bin/llvm-nm" -D --defined-only "$(hostpath "$out")" 2>/dev/null | grep -qw zygisk_module_entry; then
            die "в $out отсутствует экспортируемая точка входа zygisk_module_entry"
        fi
        if "$TOOLCHAIN/bin/llvm-nm" -D --defined-only "$(hostpath "$out")" 2>/dev/null | grep -qw zygisk_companion_entry; then
            ok "$abi: точки входа на месте (module + companion)"
        else
            ok "$abi: точка входа на месте (module)"
        fi

        built+=("$out")

        # ---------------------------------------------------- утилиты варианта
        #
        # Патчеры vold работают с памятью процесса и разбирают ELF сами, так что
        # их ABI не обязан совпадать с ABI vold. Собираются под ту же
        # архитектуру, что storage-fix, просто чтобы в архиве не было лишнего.
        [[ ${#ctools[@]} -eq 0 ]] && continue

        cc="$TOOLCHAIN/bin/${prefix}${API}-clang"
        [[ -x "$cc" ]] || die "нет компилятора $cc (проверьте API=$API)"
        suffix="$(abi_suffix_for "$abi")" || die "нет суффикса для $abi"
        mkdir -p "$vdir/tools"

        for t in "${ctools[@]}"; do
            tout="$vdir/tools/$t-$suffix"
            info "сборка $abi -> $(basename "$tout")"

            "$cc" "${CFLAGS[@]}" "$(hostpath "$HERE/tools/$t.c")" \
                -o "$(hostpath "$tout")" -Wl,--gc-sections -Wl,--build-id=none
            strip_all "$tout"

            ok "$abi: $(basename "$tout") — $(wc -c < "$tout") байт"
            built+=("$tout")
        done
    done
done

# --------------------------------------------------------------- zip
if [[ "$ZIP" == "1" ]]; then
    for variant in "${VARIANTS[@]}"; do
        pack "$variant"
    done
fi

printf '\n'
info "собрано файлов: ${#built[@]}"
for f in "${built[@]}"; do printf '    %s\n' "$f"; done
