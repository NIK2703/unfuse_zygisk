#!/system/bin/sh
#
# storage.sh — relabel /data/media and give apps ACLs on the raw tree (from
# post-fs-data.sh and service.sh). With neither sdcardfs nor FUSE handing out
# permissions, the ACL for group 9997 is what grants access.

MODDIR="${MODDIR:-${0%/*}}"

# media_userdir_file gives appdomain one search and no getattr (domain.te:252),
# so stat()/ls() on /storage/emulated return EACCES with no AVC; the contents
# already carry media_rw_data_file, where appdomain has full rights (app.te:149).
cur=$(ls -Zd /data/media 2>/dev/null | awk '{print $1}')
case "$cur" in
    *:media_rw_data_file:*) ;;
    *) chcon u:object_r:media_rw_data_file:s0 /data/media 2>/dev/null ;;
esac

# /data/media is 1023:1023 and apps are not in group 1023, so FUSE handed out
# 0770/0660 for gid 9997 (AID_EVERYBODY, android_filesystem_config.h:166).
#
# Named entries, not chmod: vold resets owner and mode of /data/media and the
# Android* dirs every boot, and chmod touches only USER_OBJ/GROUP_OBJ/MASK/
# OTHER; a default ACL also makes the kernel skip umask. vold appends default
# ACLs after the service stage — tools/vold-noacl.c stops that.
FIX="$MODDIR/tools/storage-fix"
[ -x "$FIX" ] || exit 1

"$FIX" --traverse /data/media >/dev/null 2>&1

# /data/media/0 is absent on a not-yet-expanded volume; service.sh retries.
if [ -d /data/media/0 ]; then
    "$FIX" /data/media/0 >/dev/null 2>&1
fi

if [ -d /data/media/obb ]; then
    "$FIX" /data/media/obb >/dev/null 2>&1
fi

exit 0
