#!/system/bin/sh
#
# storage.sh — prepares internal storage for the module. Runs twice, from
# post-fs-data.sh (before Zygote) and from service.sh (after vold); both passes
# are idempotent.
#
# One path only: sdcardfs on /mnt/runtime/*/emulated (step 2), preceded by the
# label of the /data/media root (step 1). There is no alternative and no
# fallback — the installer refuses the module on a kernel without sdcardfs, so
# anything less than a working sdcardfs mount here means the module has nothing
# to offer and says so in the log.
#

STAGE="${1:-storage}"
MODDIR="${MODDIR:-${0%/*}}"
LOG=/data/adb/unfuse_zygisk.log

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $STAGE: $*" >> "$LOG"; }

# Общие примитивы обеих сборок: перемаркировка корня /data/media.
. "$MODDIR/lib.sh"

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
#
# Сама работа — в lib.sh (unfuse_relabel_media_root); здесь остаётся только то,
# чего у unfuse нет: запись в лог. Коды возврата: 0 — метка уже была нужной,
# 1 — перемаркировали, 2 — chcon не прошёл. stderr самого chcon идёт в тот же лог.
prev=$(unfuse_relabel_media_root "$LOG")
case $? in
    0) ;;
    1) log "/data/media: $prev -> media_rw_data_file" ;;
    2) log "/data/media: НЕ УДАЛОСЬ перемаркировать (осталось $prev)" ;;
esac

# --- 2. sdcardfs on /mnt/runtime/*/emulated ------------------------------------
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

# Points and what is requested at each: <name>:<mask>:<gid>. The single source
# of truth for both mounting and verification, so the check cannot drift from
# what mount(2) was asked for. mask is decimal (see above).
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

# --- 2a. Verify by fact --------------------------------------------------------
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

# --- 2b. Step 2 proper --------------------------------------------------------
#
# Without a kernel sdcardfs there is nothing to mount: the installer already
# refuses the module, so a kernel that lost it mid-session is the only way here.
# A failed verification is not something to work around either — the mounts are
# left as they are and the failure is recorded.
#
if ! grep -qw sdcardfs /proc/filesystems 2>/dev/null; then
    log "в ядре нет sdcardfs (/proc/filesystems) — основной путь недоступен"
    exit 1
fi

mount_all

if sdcardfs_verify; then
    log "основной путь готов и проверен: sdcardfs на /mnt/runtime/*/emulated"
    exit 0
fi

log "основной путь не прошёл проверку — оставляю как есть, альтернативы нет"
exit 1
