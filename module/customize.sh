# shellcheck shell=sh
#
# customize.sh — called by the Magisk/KernelSU installer while unpacking the
# module: checks the build is present, keeps the tool binaries for this
# architecture, sets permissions, and prints the one line described at the
# bottom.
#

SKIPUNZIP=0

case "$ARCH" in
    arm64) ABI=arm64-v8a;   FIX=storage-fix-arm64; NOACL=vold-noacl-arm64; FUSEFS=vold-fusefs-arm64 ;;
    arm)   ABI=armeabi-v7a; FIX=storage-fix-arm;   NOACL=vold-noacl-arm;   FUSEFS=vold-fusefs-arm   ;;
    *)     abort "! Unsupported architecture: $ARCH (arm64-v8a or armeabi-v7a required)" ;;
esac

if [ ! -f "$MODPATH/zygisk/$ABI.so" ]; then
    abort "! zygisk/$ABI.so is missing - the build did not run (build.sh)"
fi

# The archive carries all three tools for every ABI: keep only ours and rename
# them — storage.sh, post-fs-data.sh and service.sh look for these names.
if [ ! -f "$MODPATH/tools/$FIX" ]; then
    abort "! tools/$FIX is missing - the build did not run (build.sh)"
fi
if [ ! -f "$MODPATH/tools/$NOACL" ]; then
    abort "! tools/$NOACL is missing - the build did not run (build.sh)"
fi
if [ ! -f "$MODPATH/tools/$FUSEFS" ]; then
    abort "! tools/$FUSEFS is missing - the build did not run (build.sh)"
fi
mv "$MODPATH/tools/$FIX"    "$MODPATH/tools/storage-fix"
mv "$MODPATH/tools/$NOACL"  "$MODPATH/tools/vold-noacl"
mv "$MODPATH/tools/$FUSEFS" "$MODPATH/tools/vold-fusefs"
rm -f "$MODPATH/tools/storage-fix-arm64" "$MODPATH/tools/storage-fix-arm" \
      "$MODPATH/tools/vold-noacl-arm64"  "$MODPATH/tools/vold-noacl-arm" \
      "$MODPATH/tools/vold-fusefs-arm64" "$MODPATH/tools/vold-fusefs-arm"

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/zygisk/$ABI.so"     0 0 0644
set_perm "$MODPATH/tools/storage-fix"  0 0 0755
set_perm "$MODPATH/tools/vold-noacl"   0 0 0755
set_perm "$MODPATH/tools/vold-fusefs"  0 0 0755
set_perm "$MODPATH/customize.sh"       0 0 0755 2>/dev/null
set_perm "$MODPATH/post-fs-data.sh"    0 0 0755 2>/dev/null
set_perm "$MODPATH/service.sh"         0 0 0755 2>/dev/null
set_perm "$MODPATH/storage.sh"         0 0 0755 2>/dev/null
set_perm "$MODPATH/status.sh"          0 0 0755 2>/dev/null

# The description line in module.prop gets a status prefix written into it at
# boot (status.sh). The shipped module.prop already carries the plain text, so
# a fresh install starts with a description that is simply missing its status
# rather than one left over from the build machine.
set_perm "$MODPATH/description.txt" 0 0 0644 2>/dev/null

# Legacy copies of the module's old config file. Nothing reads them any more —
# this branch has no mode — so a module left over from an older build cannot
# influence the new one.
rm -f /data/adb/unfuse_zygisk.conf 2>/dev/null
rm -f "$MODPATH/unfuse_zygisk.conf" 2>/dev/null

# --- one line of output ------------------------------------------------------
#
# The installer says exactly one thing: that the module cuts vold's FUSE mount
# off and opens the raw /data/media tree with an ACL for group 9997. What the
# module does and how it is doing is in the description of module.prop, which
# service.sh rewrites with a status prefix on every boot; printing it here as
# well would only bury the one fact that cannot be read there before the first
# boot.
#
ui_print "- Storage path: raw /data/media, FUSE off in vold"
