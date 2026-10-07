#!/system/bin/sh
#
# post-fs-data.sh — prepare storage before Zygote starts.
#
# The Zygisk module mounts nothing — it only clears mount_storage_dirs at
# specialisation. Storage reaches the apps through vold, so by the time the first
# app launches these must already exist:
#
#   * the media_rw_data_file label on the /data/media root;
#   * the ACL for group 9997 on the raw /data/media tree;
#   * vold's FUSE mount for emulated storage redirected away, so the apps reach
#     the raw tree rather than FUSE.
#
# storage.sh does the first two. The third has to happen before vold prepares
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

# --- cut FUSE off in vold ----------------------------------------------------
#
# The deepest thing this branch does: instead of living alongside the FUSE mount
# and fighting it for permissions (which is what the ACL work in storage.sh
# does), vold's own mount() trampoline is redirected so that MountUserFuse()
# ends with a bind of /data/media on top of the FUSE mount it asked for. The
# FUSE superblock still exists — the fd it hands to MediaProvider is part of
# its contract, and a volume whose daemon does not start is reported
# "unmountable" — but nothing reaches it, because the bind is the topmost mount
# at the path.
#
# The same run redirects vold's umount2() trampoline too. That is not a
# separate feature: vold's teardown removes exactly one mount per path, so
# without it UnmountUserFuse() would take our bind off and leave the FUSE mount
# behind, and every later mount of the volume would fail with ENOTCONN. The two
# hooks go in together or not at all.
#
# The patch lives in process memory, so it is re-applied every boot; it is
# idempotent — on an already patched vold the tool confirms the patch instead of
# writing. Rationale for the patch itself: see the header of tools/vold-fusefs.c.
#
FUSEFS="$MODDIR/tools/vold-fusefs"
if [ ! -x "$FUSEFS" ]; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: нет $FUSEFS —" \
        "vold смонтирует FUSE, прямого доступа не будет" >>"$LOG"
elif "$FUSEFS" --wait 3 >>"$LOG" 2>&1; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: FUSE отключён в vold" >>"$LOG"
else
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: vold-fusefs вернул $? —" \
        "FUSE останется смонтированным" >>"$LOG"
fi

# --- stop vold from writing the default ACL for group 1023 --------------------
#
# vold prepares the user's CE storage later than this stage: here it only does
# DE (vold-16/FsCrypt.cpp:657) and leaves CE to the framework — "the framework
# will prepare the user's CE storage later, once their CE key is installed".
# So we get in before its SetDefaultAcl(media_ce_path, ...) call.
#
# Ordered after the FUSE patch on purpose: both patch vold's memory, and the FUSE
# redirection changes which mount() calls happen at all. Applying them in this
# order means the ACL pass has already been neutralised regardless.
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
