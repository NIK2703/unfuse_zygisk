#!/system/bin/sh
#
# service.sh — repeat the preparation after vold mounts the storages, then
# report the state.
#
# vold may have restored the media_userdir_file label while preparing
# /data/media, so storage.sh runs again after it starts. The pass is idempotent.
#

MODDIR=${MODDIR:-${0%/*}}

sh "$MODDIR/storage.sh"

# The sign in the description. It reads only /proc/filesystems, so it does not
# care that the storage pass above ran first.
sh "$MODDIR/status.sh"
