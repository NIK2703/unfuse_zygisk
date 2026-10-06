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
