#!/system/bin/sh
# service.sh — late-boot stage: after vold has mounted the storages, repeat the
# preparation (storage.sh). vold may restore the media_userdir_file label on
# /data/media while preparing it, so that pass runs again here; it is idempotent.
#
# No journal: the stage leaves nothing behind but its exit code, and the answer
# that used to be written here is the same one status.sh puts into module.prop.

MODDIR=${MODDIR:-${0%/*}}

sh "$MODDIR/storage.sh"

# vold-noacl: the patch lives in vold's process memory only, and vold may not
# exist at post-fs-data, so it is repeated here; idempotent. No catch-up ACL
# passes: the trampoline address comes from vold's tables (tools/vold-noacl.c) —
# nothing to wait for, and silent retries would only mask a failure.
NOACL="$MODDIR/tools/vold-noacl"
[ -x "$NOACL" ] && "$NOACL" >/dev/null 2>&1

# vold-fusefs: same reason — the patch lives in vold's memory only and the
# framework may have restarted vold since post-fs-data; idempotent. A failure is
# app-visible: vold mounts FUSE over the raw tree again — which is what
# status.sh's description line answers.
FUSEFS="$MODDIR/tools/vold-fusefs"
[ -x "$FUSEFS" ] && "$FUSEFS" >/dev/null 2>&1

# status.sh reads the same patch state as the block above; it also runs alone.
sh "$MODDIR/status.sh"
