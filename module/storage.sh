#!/system/bin/sh
#
# storage.sh — prepares internal storage for the module, twice: from
# post-fs-data.sh (before Zygote) and from service.sh (after vold), both passes
# idempotent. One path only: vold's FUSE mount is cut off (tools/vold-fusefs)
# and the raw /data/media tree is opened to apps with an ACL for group 9997 —
# the thing that grants access, since neither sdcardfs nor FUSE is left to hand
# out permissions.
#
# Nothing is written down: the module keeps no journal. What this pass produced
# is read back with tools/storage-fix --check, and the FUSE patch's state with
# tools/vold-fusefs --check (module/status.sh).

MODDIR="${MODDIR:-${0%/*}}"

# 1. Relabel the /data/media root: media_userdir_file -> media_rw_data_file
#
# /data/media sits under /storage/emulated (as AOSP does on /mnt/pass_through,
# vold-16/Utils.cpp:1691), so its root must carry a label appdomain can search:
# on media_userdir_file appdomain and coredomain get one search and no getattr
# (domain.te:252) — stat()/ls() on /storage/emulated then return EACCES, no AVC.
# The contents carry media_rw_data_file either way, where appdomain has full
# rights (app.te:149). No sepolicy patch is needed: relabelto is neverallow for
# the zygote domain (domain.te:791), so the relabel happens here.
cur=$(ls -Zd /data/media 2>/dev/null | awk '{print $1}')
case "$cur" in
    *:media_rw_data_file:*) ;;
    *) chcon u:object_r:media_rw_data_file:s0 /data/media 2>/dev/null ;;
esac

# 2. The one path: ACLs on the raw tree
#
# /data/media is owned by 1023:1023 with modes 0550/2770/0670 and apps are not
# in group 1023 — they could not even enter the root. FUSE would have handed out
# 0770/0660 for gid 9997 (AID_EVERYBODY, android_filesystem_config.h:166).
#
# Named entries, not chmod: vold resets owner and mode of /data/media, Android,
# Android/data, Android/obb and Android/media every boot (fs_prepare_dir ->
# chown+chmod), and chmod touches only USER_OBJ/GROUP_OBJ/MASK/OTHER. Named
# entries also survive the apps' umask 0077 — with a default ACL present the
# kernel skips umask entirely — so new files come out 0660 with the inherited
# 9997 entry. Repeats do not close the race: vold appends the default ACL of
# /data/media/<user> and of package dirs AFTER the service stage, so the patch
# does (tools/vold-noacl.c makes vold::SetDefaultAcl() a no-op).
FIX="$MODDIR/tools/storage-fix"
[ -x "$FIX" ] || exit 1

# The volume root gets r-x only: traversable, nothing is written there.
"$FIX" --traverse /data/media >/dev/null 2>&1

# /data/media/0 is absent on a not-yet-expanded volume; service.sh runs this
# again later, which is why the miss is not a failure here.
if [ -d /data/media/0 ]; then
    "$FIX" /data/media/0 >/dev/null 2>&1
fi

# Legacy OBB outside the user directory (unshared_obb).
if [ -d /data/media/obb ]; then
    "$FIX" /data/media/obb >/dev/null 2>&1
fi

exit 0
