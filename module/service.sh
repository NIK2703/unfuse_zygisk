#!/system/bin/sh
# service.sh — late boot, once vold has mounted the storages: repeat the
# preparation (all passes idempotent).

MODDIR=${MODDIR:-${0%/*}}

# vold may restore the media_userdir_file label on /data/media while preparing
# it, so the relabel runs again here.
sh "$MODDIR/storage.sh"

# Both vold patches live in vold's process memory and vold may not have existed
# at post-fs-data, so repeat them. A FUSE failure is app-visible: vold mounts
# FUSE over the raw tree again.
NOACL="$MODDIR/tools/vold-noacl"
[ -x "$NOACL" ] && "$NOACL" >/dev/null 2>&1

FUSEFS="$MODDIR/tools/vold-fusefs"
[ -x "$FUSEFS" ] && "$FUSEFS" >/dev/null 2>&1

sh "$MODDIR/status.sh"
