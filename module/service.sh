#!/system/bin/sh
#
# service.sh — repeat the preparation after vold mounts the storages.
#
# vold may have restored the media_userdir_file label while preparing
# /data/media, so both storage.sh steps run again after it starts. They are
# idempotent.
#

MODDIR=${MODDIR:-${0%/*}}
LOG=/data/adb/unfuse_zygisk.log
stamp() { date '+%Y-%m-%d %H:%M:%S'; }

sh "$MODDIR/storage.sh" service

# --- vold patch: confirmation -------------------------------------------------
#
# Repeated because the patch lives in process memory only, and at the
# post-fs-data stage vold may not have existed yet. The step is idempotent, so
# the repeat costs nothing: on an already patched vold the tool simply confirms
# the patch is in place.
#
# There is deliberately no fallback of "repeated ACL passes" here. That was
# needed while the patch could fail to install on a foreign vold build: vold had
# to be chased with three passes at 15/30/75 seconds after boot, hidden from the
# log. Now the trampoline address is derived from vold's own tables
# (tools/vold-noacl.c), there is nothing to decline, and silent catch-up passes
# would only mask a failure. If the patch does not install, the log says so, and
# that is what needs fixing.
#
NOACL="$MODDIR/tools/vold-noacl"
if [ ! -x "$NOACL" ]; then
    echo "[$(stamp)] service: нет $NOACL — vold перепишет default-ACL у /data/media/0" >>"$LOG"
elif "$NOACL" >>"$LOG" 2>&1; then
    echo "[$(stamp)] service: патч vold на месте — vold не пишет default-ACL" >>"$LOG"
else
    echo "[$(stamp)] service: патч vold НЕ встал — vold перепишет default-ACL у /data/media/0" >>"$LOG"
fi

# --- FUSE-off patch: confirmation (mode=fuse) --------------------------------
#
# Repeated for the same reason as vold-noacl: the patch lives in vold's memory
# only, and at the post-fs-data stage vold may not have existed yet, or may have
# been restarted by the framework since. The step is idempotent.
#
# Unlike vold-noacl, a failure here is visible rather than merely inconvenient:
# if the patch is not in place, vold mounts FUSE and the mode silently reverts to
# the acl behaviour. So the log line says explicitly which of the two happened,
# and path-mode.sh reports the same thing on demand.
#
FUSEMODE=$(sed -n 's/^[[:space:]]*path[[:space:]]*=[[:space:]]*//p' \
    "$MODDIR/unfuse_zygisk.conf" 2>/dev/null \
    | sed 's/[[:space:]]*#.*$//' | sed 's/[[:space:]]*$//' | tail -n 1)
case "$FUSEMODE" in
    raw) FUSEMODE=acl ;;
esac
[ -n "$FUSEMODE" ] || FUSEMODE=auto

if [ "$FUSEMODE" = fuse ]; then
    FUSEFS="$MODDIR/tools/vold-fusefs"
    if [ ! -x "$FUSEFS" ]; then
        echo "[$(stamp)] service: нет $FUSEFS — FUSE останется смонтированным" >>"$LOG"
    elif "$FUSEFS" >>"$LOG" 2>&1; then
        echo "[$(stamp)] service: FUSE отключён в vold — MountUserFuse перехвачен" >>"$LOG"
    else
        echo "[$(stamp)] service: патч FUSE НЕ встал — vold смонтирует FUSE," \
            "режим выродится в acl" >>"$LOG"
    fi
fi
