#!/system/bin/sh
#
# storage.sh — internal storage for the module: relabel /data/media and give
# apps ACLs on the raw tree (from post-fs-data.sh and service.sh, idempotent).
# With neither sdcardfs nor FUSE left to hand out permissions, the ACL for
# group 9997 is what grants access.

MODDIR="${MODDIR:-${0%/*}}"

# 1. Relabel the /data/media root: media_userdir_file -> media_rw_data_file
#
# media_userdir_file gives appdomain one search and no getattr (domain.te:252),
# so stat()/ls() on /storage/emulated return EACCES with no AVC; the contents
# carry media_rw_data_file, where appdomain has full rights (app.te:149).
cur=$(ls -Zd /data/media 2>/dev/null | awk '{print $1}')
case "$cur" in
    *:media_rw_data_file:*) ;;
    *) chcon u:object_r:media_rw_data_file:s0 /data/media 2>/dev/null ;;
esac

# 2. ACLs on the raw tree
#
# /data/media is 1023:1023, modes 0550/2770/0670, and apps are not in group
# 1023 — they could not enter the root. FUSE would have handed out 0770/0660 for
# gid 9997 (AID_EVERYBODY, android_filesystem_config.h:166).
#
# Named entries, not chmod: vold resets owner and mode of /data/media and the
# Android* dirs every boot, and chmod touches only USER_OBJ/GROUP_OBJ/MASK/
# OTHER; a default ACL also makes the kernel skip umask. vold appends default
# ACLs after the service stage — tools/vold-noacl.c stops that.
FIX="$MODDIR/tools/storage-fix"
[ -x "$FIX" ] || exit 1

# Volume root: r-x only — traversable, nothing is written there.
"$FIX" --traverse /data/media >/dev/null 2>&1

# /data/media/0 is absent on a not-yet-expanded volume; service.sh runs this
# again later, so the miss is not a failure here.
if [ -d /data/media/0 ]; then
    "$FIX" /data/media/0 >/dev/null 2>&1
fi

# Legacy OBB outside the user directory (unshared_obb).
if [ -d /data/media/obb ]; then
    "$FIX" /data/media/obb >/dev/null 2>&1
fi

exit 0
