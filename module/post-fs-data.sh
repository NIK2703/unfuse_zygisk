#!/system/bin/sh
#
# post-fs-data.sh — prepare storage before Zygote starts, in order: state dir,
# storage.sh, FUSE patch, vold-noacl. All four must exist before the first app
# launches; storage reaches the apps through vold.
#
# No stage here writes a journal: the module has no log. What a stage leaves
# behind is its exit code and the patch state, which tools/status.sh reads.
#
# The FUSE mount is still made (its fd is the contract with MediaProvider's
# daemon) but nothing reaches it: tools/vold-fusefs redirects vold's mount()
# trampoline, so MountUserFuse() ends with a bind of the raw /data/media tree
# on top. Design and rationale: docs/fuse-root-patch-design.md, "Где это
# встраивается в модуль".

MODDIR=${MODDIR:-${0%/*}}

# Holds `once` (boot marker, claimed by the first app that gets storage) and
# `no_hooks` (manual switch for the libc patching off). Own directory, not
# /data/adb: the zygote domain there only gets traversal (allow zygote
# adb_data_file dir search, zygisksu/sepolicy.rule), so open(O_CREAT) fails on
# add_name/write and is dontaudit'ed — no logcat, no avc (see the header of
# src/unfuse_zygisk.cpp). Its own magisk_file dir works instead: allow *
# magisk_file dir * / file * is the type the policy hands every domain, and a
# file created inside inherits the label (verified on device).
#
# mkdir, not rm -rf: no_hooks is the user's file and has to survive a reboot;
# the label is re-applied every boot because nothing guarantees it survived the
# last one. Not repeated in service.sh, unlike the vold patches: no vold
# dependency — if the directory is not there now, it never will be.
#
STATE=/data/adb/unfuse_zygisk.state
mkdir -p "$STATE" 2>/dev/null
chcon u:object_r:magisk_file:s0 "$STATE" 2>/dev/null

# `once` must be gone before Zygote starts, or the marker stays absent for the
# whole boot after the first one; not in service.sh, apps launch by then.
rm -f "$STATE/once" 2>/dev/null

sh "$MODDIR/storage.sh"

# tools/vold-fusefs redirects vold's mount() trampoline, so MountUserFuse()
# ends with a bind of /data/media on top of the FUSE mount it asked for; that
# mount stays — its fd is MediaProvider's contract, and a volume whose daemon
# does not start is reported "unmountable" — but nothing reaches it. The same
# run redirects umount2, and both hooks go in together: vold's teardown removes
# exactly one mount per path, so without it UnmountUserFuse() would take our
# bind off, leave the FUSE mount behind, and every later mount fails with
# ENOTCONN. Lives in process memory: re-applied every boot, idempotent — on a
# patched vold the tool just confirms. See the header of tools/vold-fusefs.c.
FUSEFS="$MODDIR/tools/vold-fusefs"
[ -x "$FUSEFS" ] && "$FUSEFS" --wait 3 >/dev/null 2>&1

# vold prepares the user's CE storage later than this stage: here it only does
# DE (vold-16/FsCrypt.cpp:657) and leaves CE to the framework, so we get in
# before its SetDefaultAcl(media_ce_path, ...) call. After the FUSE patch on
# purpose — both patch vold's memory, and the FUSE redirection removes the
# mount() calls the ACL pass needs anyway. Process memory, so re-applied every
# reboot: here and, just in case, in service.sh; both idempotent. See the header
# of tools/vold-noacl.c.
NOACL="$MODDIR/tools/vold-noacl"
[ -x "$NOACL" ] && "$NOACL" --wait 3 >/dev/null 2>&1
