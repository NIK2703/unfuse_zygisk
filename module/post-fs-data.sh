#!/system/bin/sh
# post-fs-data.sh — storage before Zygote starts: state dir, storage.sh, the
# vold FUSE patch, then vold-noacl. Design: docs/fuse-root-patch-design.md.

MODDIR=${MODDIR:-${0%/*}}

# State dir: `once` is the boot marker the first app with storage claims;
# `no_hooks` switches libc patching off. Not /data/adb — the zygote domain gets
# traversal only there, so open(O_CREAT) fails, while a file under magisk_file
# inherits a label every domain may write. mkdir, not rm -rf: no_hooks is the
# user's file and must survive a reboot.
STATE=/data/adb/unfuse_zygisk.state
mkdir -p "$STATE" 2>/dev/null
chcon u:object_r:magisk_file:s0 "$STATE" 2>/dev/null

# Must be gone before Zygote starts, or it stays absent for the whole boot.
rm -f "$STATE/once" 2>/dev/null

sh "$MODDIR/storage.sh"

# vold-fusefs redirects vold's mount() trampoline, so MountUserFuse() ends with
# a bind of raw /data/media over the FUSE mount it asked for. The FUSE mount
# stays — its fd is MediaProvider's contract, and a volume whose daemon never
# starts is reported "unmountable" — but nothing reaches it. umount2 goes in the
# same run: vold's teardown removes one mount per path, so without it our bind
# would come off and every later mount would fail with ENOTCONN. Idempotent.
FUSEFS="$MODDIR/tools/vold-fusefs"
[ -x "$FUSEFS" ] && "$FUSEFS" --wait 3 >/dev/null 2>&1

# vold only does DE this early (vold-16/FsCrypt.cpp:657) and leaves CE to the
# framework, so we get in before its SetDefaultAcl(media_ce_path, ...). After
# the FUSE patch on purpose: both patch vold's memory, and the redirection
# removes the mount() calls the ACL pass needs anyway. Repeated in service.sh.
NOACL="$MODDIR/tools/vold-noacl"
[ -x "$NOACL" ] && "$NOACL" --wait 3 >/dev/null 2>&1
