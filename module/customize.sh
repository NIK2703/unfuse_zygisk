# shellcheck shell=sh
# customize.sh — install-time stage: the Magisk/KernelSU installer runs it while
# unpacking the module.

SKIPUNZIP=0

case "$ARCH" in
    arm64) ABI=arm64-v8a;   FIX=storage-fix-arm64; NOACL=vold-noacl-arm64; FUSEFS=vold-fusefs-arm64 ;;
    arm)   ABI=armeabi-v7a; FIX=storage-fix-arm;   NOACL=vold-noacl-arm;   FUSEFS=vold-fusefs-arm   ;;
    *)     abort "! Unsupported architecture: $ARCH (arm64-v8a or armeabi-v7a required)" ;;
esac

if [ ! -f "$MODPATH/zygisk/$ABI.so" ]; then
    abort "! zygisk/$ABI.so is missing - the build did not run (build.sh)"
fi

# The archive carries all three tools for every ABI: keep only ours, renamed to
# the names storage.sh, post-fs-data.sh and service.sh look for.
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

# status.sh rewrites module.prop's description at every boot; the shipped one
# carries plain text, so a fresh install shows no status left from the build.
set_perm "$MODPATH/description.txt" 0 0 0644 2>/dev/null

# Legacy config files: nothing reads them; old leftovers cannot interfere.
rm -f /data/adb/unfuse_zygisk.conf 2>/dev/null
rm -f "$MODPATH/unfuse_zygisk.conf" 2>/dev/null

# One line, not the description: module.prop already says what the module does.
ui_print "- Storage path: raw /data/media, FUSE off in vold"
