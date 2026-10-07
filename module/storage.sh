#!/system/bin/sh
#
# storage.sh — prepares internal storage for the module. Runs twice, from
# post-fs-data.sh (before Zygote) and from service.sh (after vold); both passes
# are idempotent.
#
# Two paths: the main one brings up sdcardfs on /mnt/runtime/*/emulated (step 2),
# the fallback places ACLs on the raw /data/media tree (step 3). Step 0 picks
# between them from unfuse_zygisk.conf in the module directory (key path,
# default auto).
#

STAGE="${1:-storage}"
MODDIR="${MODDIR:-${0%/*}}"
LOG=/data/adb/unfuse_zygisk.log

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $STAGE: $*" >> "$LOG"; }

# --- 0. Which path to use: the module config ---------------------------------
#
# The choice is a module setting, not state of this boot: it sits next to the
# scripts as $MODDIR/unfuse_zygisk.conf and survives a reboot. The module
# directory is rewritten whole on every install, so customize.sh carries the
# previous mode over into the fresh copy. The file is parsed, never sourced —
# anything with root can write it, and the module must not execute code from its
# own directory. Unknown or empty means auto: a typo must not leave the module
# without storage.
#
CONF="$MODDIR/unfuse_zygisk.conf"

# Value of key $1. The last occurrence wins, as in any config file.
conf_get() {
    [ -r "$CONF" ] || return 1
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$CONF" 2>/dev/null \
        | sed 's/[[:space:]]*#.*$//' | sed 's/[[:space:]]*$//' | tail -n 1
}

PATH_MODE=$(conf_get path)
PATH_SRC="конфиг $CONF"

if [ -z "$PATH_MODE" ]; then
    PATH_MODE=auto
    PATH_SRC="по умолчанию (в конфиге нет path)"
fi

case "$PATH_MODE" in
    auto|sdcardfs|acl|fuse) ;;
    raw)
        # Former name of the acl mode; handled as acl from here on.
        log "конфиг: path=raw — прежнее имя режима acl"
        PATH_MODE=acl
        ;;
    *)
        bad="$PATH_MODE"
        log "конфиг: неизвестное path=$bad — беру auto"
        PATH_MODE=auto
        PATH_SRC="по умолчанию (path=$bad не распознан)"
        ;;
esac

# Legacy synonym of path=acl: a marker file that enabled the fallback before the
# config existed. Kept for old builds and third-party instructions. An explicit
# path wins: a conflict is logged, not silently obeyed.
LEGACY_RAW=/data/adb/unfuse_zygisk.force_raw
if [ -e "$LEGACY_RAW" ]; then
    if [ "$PATH_SRC" = "конфиг $CONF" ]; then
        log "конфиг: $LEGACY_RAW на месте, но конфиг задаёт path=$PATH_MODE — конфиг главнее"
    else
        PATH_MODE=acl
        PATH_SRC="устаревший $LEGACY_RAW"
    fi
fi

log "режим пути: $PATH_MODE ($PATH_SRC)"

# --- 1. Relabel the /data/media root: media_userdir_file -> media_rw_data_file
#
# sdcardfs forwards getattr() on the mount root to the lower inode (/data/media).
# On media_userdir_file appdomain and coredomain get one search and no getattr
# (domain.te:252), so stat()/ls() on /storage/emulated return EACCES — with no
# AVC at all, the denial is marked dontaudit. The contents are fine either way:
# they carry media_rw_data_file, where appdomain has full rights (app.te:149).
#
# No sepolicy patch is needed: the permission already exists, and relabelto is
# neverallow for the zygote domain (domain.te:791), so the relabel happens here.
# On the fallback path it is mandatory: the /data/media tree itself sits under
# /storage/emulated, and its root must carry this label to be searchable.
#
cur=$(ls -Zd /data/media 2>/dev/null | awk '{print $1}')
case "$cur" in
    *:media_rw_data_file:*) ;;
    *)
        if chcon u:object_r:media_rw_data_file:s0 /data/media 2>>"$LOG"; then
            log "/data/media: $cur -> media_rw_data_file"
        else
            log "/data/media: НЕ УДАЛОСЬ перемаркировать (осталось $cur)"
        fi
        ;;
esac

# --- 2. Main path: sdcardfs on /mnt/runtime/*/emulated -----------------------
#
# external_storage.sdcardfs.enabled=0 on this firmware (in /vendor/build.prop),
# so vold never runs /system/bin/sdcard and those points stay empty directories.
#
# Why not /system/bin/sdcard: AOSP's sdcard.cpp mounts only default/emulated with
# a real mount(2) and derives read/write/full via bind + MS_REMOUNT. Here
# sdcardfs_remount_fs() ignores options (a stub) and vfsopts live in the
# superblock, so all four would inherit default's options (gid=1015, mask=6).
# Each point therefore gets its own mount(2), hence its own anonymous superblock.
#
# mask is decimal: sdcard.cpp does StringPrintf("mask=%d", mask) over an octal
# literal, so 0006 -> 6, 0027 -> 23, 0007 -> 7. full: mask=0007 with gid=9997
# (AID_EVERYBODY) gives 0770 on directories and 0660 on files for any process.
#
SDCARDFS_FS=sdcardfs

# Filesystem type effectively mounted at a point: the LAST /proc/mounts entry
# wins, since mounts stack and the topmost is last. A grep would also see what
# lies under the point and wrongly report sdcardfs. Shell only, no stat(1): on
# module stages KernelSU injects busybox, whose `stat -c %T` prints UNKNOWN
# instead of the magic number (toybox gives 0x5dca2df5).
fs_type() {
    t=""
    while read -r _dev mp ty _rest; do
        [ "$mp" = "$1" ] && t="$ty"
    done < /proc/mounts
    echo "$t"
}

COMMON="fsuid=1023,fsgid=1023,multiuser,derive_gid,default_normal,unshared_obb,userid=0"

# Main-path points and what is requested at each: <name>:<mask>:<gid>. The single
# source of truth for both mounting and verification, so the check cannot drift
# from what mount(2) was asked for. mask is decimal (see above).
#
SDCARDFS_POINTS="default:6:1015 read:23:9997 write:7:9997 full:7:9997"

# "<type> <options>" for a point as the kernel shows them; empty if absent. Last
# entry wins, as in fs_type().
mount_line() {
    r=""
    while read -r _dev mp ty opts _rest; do
        [ "$mp" = "$1" ] && r="$ty $opts"
    done < /proc/mounts
    [ -n "$r" ] && echo "$r"
}

# Is $2 among the options $1. Commas are wrapped around the list so "gid=1015"
# cannot match "fsgid=1015".
has_opt() {
    case ",$1," in
        *",$2,"*) return 0 ;;
        *)        return 1 ;;
    esac
}

mount_one() {
    p="/mnt/runtime/$1/emulated"

    # Already our mount — leave it: post-fs-data may have brought it up.
    if [ "$(fs_type "$p")" = "$SDCARDFS_FS" ]; then
        return 0
    fi

    mkdir -p "$p" 2>/dev/null
    if mount -t sdcardfs -o "$COMMON,mask=$2,gid=$3" /data/media "$p"; then
        log "OK   $p (mask=$2 gid=$3)"
    else
        log "FAIL $p (mask=$2 gid=$3)"
    fi
}

mount_all() {
    for spec in $SDCARDFS_POINTS; do
        rest="${spec#*:}"
        mount_one "${spec%%:*}" "${rest%%:*}" "${rest##*:}"
    done
}

umount_all() {
    for spec in $SDCARDFS_POINTS; do
        p="/mnt/runtime/${spec%%:*}/emulated"
        [ "$(fs_type "$p")" = "$SDCARDFS_FS" ] && umount "$p" 2>/dev/null
    done
}

# --- 2a. Verify the main path by fact ----------------------------------------
#
# mount(2) returns 0 even when the kernel accepted the mount without applying the
# vfsopts — not theory here, since sdcardfs_remount_fs() is a stub and four points
# sharing one superblock would all carry the first one's options (gid=1015,
# mask=6), with no error anywhere. So the path is accepted on fact:
#
#   1. the point really is sdcardfs;
#   2. its options carry the mask and gid that were requested.
#
# No separate superblock check is needed: /proc/mounts shows superblock options,
# so a shared superblock would already fail check 2. That check only applies if
# the kernel prints those options at all — if neither mask= nor gid= appears, the
# build does not show them, there is nothing to judge, and the point is not
# counted as failed (this kernel prints both).
#
sdcardfs_verify() {
    for spec in $SDCARDFS_POINTS; do
        name="${spec%%:*}"; rest="${spec#*:}"
        want_mask="${rest%%:*}"; want_gid="${rest##*:}"
        p="/mnt/runtime/$name/emulated"

        line=$(mount_line "$p")
        if [ -z "$line" ]; then
            log "проверка: $p не смонтирована"
            return 1
        fi
        ty="${line%% *}"; opts="${line#* }"

        if [ "$ty" != "$SDCARDFS_FS" ]; then
            log "проверка: $p под $ty, а не $SDCARDFS_FS"
            return 1
        fi

        case ",$opts," in
            *,mask=*)
                if ! has_opt "$opts" "mask=$want_mask"; then
                    log "проверка: $p с чужой маской — ждали mask=$want_mask," \
                        "ядро отдало: $opts"
                    return 1
                fi
                ;;
        esac

        case ",$opts," in
            *,gid=*)
                if ! has_opt "$opts" "gid=$want_gid"; then
                    log "проверка: $p с чужим gid — ждали gid=$want_gid," \
                        "ядро отдало: $opts"
                    return 1
                fi
                ;;
        esac
    done
    return 0
}

# --- 2b. What step 2 does ----------------------------------------------------
#
# By PATH_MODE: acl and fuse — never mount sdcardfs, tear down existing mounts at
# once (both want the raw tree, and differ only in whether vold's FUSE mount is
# also cut off, which is post-fs-data.sh's job, not this script's);
# auto — sdcardfs first, then verify, falling back on failure; sdcardfs — the
# same but strict: log what did not match and exit 1 instead of falling back.
#
if [ "$PATH_MODE" = acl ] || [ "$PATH_MODE" = fuse ]; then
    log "path=$PATH_MODE — основной путь выключен настройкой модуля"
    umount_all
elif grep -qw sdcardfs /proc/filesystems 2>/dev/null; then
    mount_all

    if sdcardfs_verify; then
        log "основной путь готов и проверен: sdcardfs на /mnt/runtime/*/emulated"
        exit 0
    fi

    if [ "$PATH_MODE" = sdcardfs ]; then
        # Strict mode: no fallback, and the mount is left alone — it is what was
        # asked for, and the log already says what did not match.
        log "path=sdcardfs: основной путь не прошёл проверку — оставляю как есть," \
            "альтернативный запрещён настройкой"
        exit 1
    fi

    log "основной путь не прошёл проверку — снимаю его и перехожу на альтернативный"
    umount_all
else
    if [ "$PATH_MODE" = sdcardfs ]; then
        log "path=sdcardfs, но в ядре нет sdcardfs (/proc/filesystems) —" \
            "приложения останутся без памяти: альтернативный путь запрещён настройкой"
        exit 1
    fi
    log "в ядре нет sdcardfs (/proc/filesystems) — основной путь недоступен"
fi

log "основной путь не поднялся — перехожу на альтернативный"

# --- 3. Fallback path: ACLs on the raw tree ----------------------------------
#
# /data/media itself ends up under /mnt/user/<user>/emulated (as AOSP does on
# /mnt/pass_through, vold-16/Utils.cpp:1691), but it is owned by 1023:1023 with
# modes 0550/2770/0670 and apps are not in group 1023 — they could not even enter
# the root. sdcardfs does that work itself, handing out 0770/0660 for gid 9997;
# here the same is expressed as ACLs. 9997 (AID_EVERYBODY) is the shared group of
# every app in the profile (android_filesystem_config.h:166) and exactly the gid
# of the sdcardfs read/write/full mounts, so all apps get access.
#
# A named ACL entry rather than chmod: vold resets the owner and mode of
# /data/media, Android, Android/data, Android/obb and Android/media on every boot
# (fs_prepare_dir -> chown+chmod), and chmod only touches USER_OBJ/GROUP_OBJ/
# MASK/OTHER, leaving named entries intact. It also survives the apps' umask of
# 0077: with a default ACL present the kernel skips umask entirely — vfs_create()
# omits `mode &= ~current_umask()` and posix_acl_create() intersects with the ACL
# — so new files come out 0660 with the inherited 9997 entry.
#
# Both stages run it because vold rebuilds its directories after post-fs-data.
# That alone is not enough: vold appends the default ACL of /data/media/<user> and
# of package directories AFTER the service stage, while preparing the user's CE
# storage. The race is closed by the vold patch, not by repeats:
# tools/vold-noacl.c makes vold::SetDefaultAcl() a no-op.
#
FIX="$MODDIR/tools/storage-fix"
if [ ! -x "$FIX" ]; then
    log "ВНИМАНИЕ: нет $FIX — приложения останутся без памяти"
    exit 1
fi

# The volume root gets r-x only: it must be traversable, nothing is written there.
"$FIX" --traverse /data/media >>"$LOG" 2>&1

if [ -d /data/media/0 ]; then
    "$FIX" /data/media/0 >>"$LOG" 2>&1
else
    log "нет /data/media/0 — раздел ещё не развёрнут, повтор на стадии service"
fi

# Legacy OBB outside the user directory (unshared_obb).
if [ -d /data/media/obb ]; then
    "$FIX" /data/media/obb >>"$LOG" 2>&1
fi

log "альтернативный путь: ACL с группой 9997 расставлены"

# --- 4. ACL status report ----------------------------------------------------
#
# Log line only, so the journal alone shows whether the pass ran to completion.
#
if [ -d /data/media/0 ]; then
    if "$FIX" --check /data/media/0 >>"$LOG" 2>&1; then
        log "ACL /data/media/0: запись 9997 на месте в обеих ACL"
    else
        log "ACL /data/media/0: записи 9997 нет в обеих ACL — см. --check в журнале"
    fi
fi

exit 0
