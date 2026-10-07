#!/system/bin/sh
#
# storage.sh — prepares internal storage for the module. Runs twice, from
# post-fs-data.sh (before Zygote) and from service.sh (after vold); both passes
# are idempotent.
#
# This branch has exactly one path: vold's FUSE mount for emulated storage is
# cut off (tools/vold-fusefs), and the raw /data/media tree is opened to apps
# with an ACL for group 9997. There is no sdcardfs here and no mode setting:
# with neither sdcardfs nor FUSE handing out permissions, the ACL pass below is
# not a fallback, it is the thing that grants access at all.
#
#   1. relabel the /data/media root to media_rw_data_file (so it is searchable);
#   2. place the ACLs on the raw tree (storage-fix);
#   3. report whether the ACLs are there;
#   4. report whether vold is still mounting FUSE.
#

STAGE="${1:-storage}"
MODDIR="${MODDIR:-${0%/*}}"
LOG=/data/adb/unfuse_zygisk.log

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $STAGE: $*" >> "$LOG"; }

# --- 1. Relabel the /data/media root: media_userdir_file -> media_rw_data_file
#
# /data/media itself sits under /storage/emulated (as AOSP does on
# /mnt/pass_through, vold-16/Utils.cpp:1691), so its root has to carry a label
# appdomain can search. On media_userdir_file appdomain and coredomain get one
# search and no getattr (domain.te:252), so stat()/ls() on /storage/emulated
# return EACCES — with no AVC at all, the denial is marked dontaudit. The
# contents are fine either way: they carry media_rw_data_file, where appdomain
# has full rights (app.te:149).
#
# No sepolicy patch is needed: the permission already exists, and relabelto is
# neverallow for the zygote domain (domain.te:791), so the relabel happens here.
# It is mandatory on this branch: the /data/media tree is what the apps reach
# through, and its root must carry this label to be searchable.
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

# --- 2. The one path: ACLs on the raw tree -----------------------------------
#
# vold's FUSE mount for emulated storage is cut off by tools/vold-fusefs (see
# post-fs-data.sh), so nothing else hands the apps their permissions.
#
# /data/media itself is owned by 1023:1023 with modes 0550/2770/0670 and apps
# are not in group 1023 — they could not even enter the root. FUSE would have
# handed out 0770/0660 for gid 9997; here the same is expressed as ACLs. 9997
# (AID_EVERYBODY) is the shared group of every app in the profile
# (android_filesystem_config.h:166), so all apps get access.
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

log "ACL с группой 9997 расставлены на /data/media"

# --- 3. ACL status report ----------------------------------------------------
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

# --- 4. FUSE status report ---------------------------------------------------
#
# The other half of this branch: is vold still mounting FUSE. vold-fusefs --check
# exits 0 only when the trampoline is redirected into our handler, 1 when there
# is no vold or none patched, 2 when the anchor did not resolve. Log line only —
# the sign in module.prop (status.sh) is what the module list shows.
#
FUSEFS="$MODDIR/tools/vold-fusefs"
if [ ! -x "$FUSEFS" ]; then
    log "нет $FUSEFS — vold смонтирует FUSE, прямой доступ не заработает"
elif "$FUSEFS" --check >/dev/null 2>&1; then
    log "FUSE отключён в vold — MountUserFuse перехвачен"
else
    log "FUSE НЕ отключён — vold смонтирует FUSE, прямой доступ не заработает"
fi

exit 0
