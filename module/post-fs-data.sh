#!/system/bin/sh
# post-fs-data.sh — storage before Zygote: state dir, storage.sh, the vold FUSE
# patch, vold-noacl. Design: docs/fuse-root-patch-design.md.

MODDIR=${MODDIR:-${0%/*}}

# `once` — boot marker the first app with storage claims; `no_hooks` — switch
# for libc patching. Not /data/adb: the zygote domain gets traversal only there
# and open(O_CREAT) fails; under magisk_file any domain may write. mkdir, not
# rm -rf — no_hooks is the user's and must survive a reboot.
STATE=/data/adb/unfuse_zygisk.state
mkdir -p "$STATE" 2>/dev/null
chcon u:object_r:magisk_file:s0 "$STATE" 2>/dev/null

# Must be gone before Zygote starts, or it stays absent for the whole boot.
rm -f "$STATE/once" 2>/dev/null

sh "$MODDIR/storage.sh"

# vold-fusefs redirects vold's mount() trampoline, so MountUserFuse() ends with
# a bind of raw /data/media over the FUSE mount. That mount stays — its fd is
# MediaProvider's contract — but nothing reaches it. umount2 goes in the same
# run: vold's teardown removes one mount per path, so without it our bind comes
# off and every later mount fails with ENOTCONN.
FUSEFS="$MODDIR/tools/vold-fusefs"
[ -x "$FUSEFS" ] && "$FUSEFS" --wait 3 >/dev/null 2>&1

# vold does only DE this early (vold-16/FsCrypt.cpp:657), so we run before its
# SetDefaultAcl(media_ce_path, ...). After the FUSE patch: the redirection
# removes the mount() calls the ACL pass needs.
NOACL="$MODDIR/tools/vold-noacl"
[ -x "$NOACL" ] && "$NOACL" --wait 3 >/dev/null 2>&1
