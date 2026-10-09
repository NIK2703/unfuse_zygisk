#!/system/bin/sh
# service.sh — late boot, after vold mounted the storages: repeat the
# preparation (idempotent).

MODDIR=${MODDIR:-${0%/*}}

# vold may restore the media_userdir_file label while preparing /data/media.
sh "$MODDIR/storage.sh"

# Both vold patches live in vold's memory and vold may not have existed at
# post-fs-data, so repeat them.
NOACL="$MODDIR/tools/vold-noacl"
[ -x "$NOACL" ] && "$NOACL" >/dev/null 2>&1

FUSEFS="$MODDIR/tools/vold-fusefs"
[ -x "$FUSEFS" ] && "$FUSEFS" >/dev/null 2>&1

sh "$MODDIR/status.sh"
