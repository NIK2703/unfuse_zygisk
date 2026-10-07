#!/system/bin/sh
#
# post-fs-data.sh — prepare storage before Zygote starts.
#
# The module can only expose a storage source to an app inside its private
# namespace, so by the time the first app launches these must already exist:
#
#   * the media_rw_data_file label on the /data/media root;
#   * the /mnt/runtime/*/emulated mounts — sdcardfs if the kernel has it, or
#     (on the fallback path) an ACL for group 9997 on the raw tree.
#
# storage.sh does all of that. It runs again from service.sh, after vold.
#
# In mode=fuse there is a third thing, and it has to happen before vold prepares
# the user's storage rather than after: tools/vold-fusefs is pointed at vold to
# redirect its mount() trampoline, so the FUSE mount for emulated storage is
# turned into a bind of the raw tree instead of being made at all.
#

MODDIR=${MODDIR:-${0%/*}}
LOG=/data/adb/unfuse_zygisk.log

# The native module logs one sample per boot and guards it with this marker, so
# it has to be gone before Zygote starts — otherwise the tag stays silent for
# the whole boot after the first one. Here, not in service.sh: that runs after
# apps are already launching.
rm -f /data/adb/unfuse_zygisk.once

sh "$MODDIR/storage.sh" post-fs-data

# --- stop vold from writing the default ACL for group 1023 --------------------
#
# vold prepares the user's CE storage later than this stage: here it only does
# DE (vold-16/FsCrypt.cpp:657) and leaves CE to the framework — "the framework
# will prepare the user's CE storage later, once their CE key is installed".
# So we get in before its SetDefaultAcl(media_ce_path, ...) call.
#
# The patch lives in process memory, so it must be re-applied after every
# reboot: here and, just in case, in service.sh. Both steps are idempotent — on
# a fresh vold the trampoline is intact, on an already patched one the tool just
# confirms the patch is in place.
#
# Rationale for the patch itself: see the header of tools/vold-noacl.c.
#
NOACL="$MODDIR/tools/vold-noacl"
if [ -x "$NOACL" ]; then
    "$NOACL" --wait 3 >>"$LOG" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: vold-noacl вернул $rc —" \
            "vold перепишет default-ACL у /data/media/0" >>"$LOG"
    fi
else
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: нет $NOACL —" \
        "vold перепишет default-ACL у /data/media/0" >>"$LOG"
fi

# --- cut FUSE off in vold (mode=fuse) ----------------------------------------
#
# The deepest of the modes: instead of living alongside the FUSE mount and
# fighting it for permissions (which is what the ACL work above does), vold's
# own mount() trampoline is redirected so that MountUserFuse() binds /data/media
# onto the target rather than mounting FUSE on it. No FUSE superblock is ever
# created for emulated storage, so there is nothing for the ACL pass to chase.
#
# Ordered after vold-noacl on purpose: both patch vold's memory, and the FUSE
# redirection changes which mount() calls happen at all. Applying them in this
# order means the ACL pass above has already been neutralised regardless.
#
# Only in mode=fuse. In auto/sdcardfs/acl the FUSE mount is left alone: those
# modes are built on coexisting with it (acl) or on replacing it with sdcardfs.
#
# The patch lives in process memory, so it is re-applied every boot; it is
# idempotent — on an already patched vold the tool confirms the patch instead of
# writing. Rationale for the patch itself: see the header of tools/vold-fusefs.c.
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
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: нет $FUSEFS —" \
            "vold смонтирует FUSE, режим fuse не работает" >>"$LOG"
    elif "$FUSEFS" --wait 3 >>"$LOG" 2>&1; then
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: FUSE отключён в vold" >>"$LOG"
    else
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: vold-fusefs вернул $? —" \
            "FUSE останется смонтированным" >>"$LOG"
    fi
fi
