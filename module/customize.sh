# shellcheck shell=sh
#
# customize.sh — called by the Magisk/KernelSU installer while unpacking the
# module: checks the build is present, keeps the tool binary for this
# architecture, sets permissions, carries the mode over from the previous
# install, and prints the one line described at the bottom.
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
set_perm "$MODPATH/path-mode.sh"       0 0 0755 2>/dev/null
set_perm "$MODPATH/status.sh"          0 0 0755 2>/dev/null

# webroot is deliberately left alone beyond the set_perm_recursive above: the
# loader sets its permissions and SELinux context at install time, and the
# KernelSU docs advise against touching it by hand. Our set_perm calls change
# owner and mode only, never the context.

# Module id — so prev_mode() below reads the module as it will be AFTER a
# reboot rather than at the install directory: $MODPATH points at
# /data/adb/modules_update/ during install, which does not work until a reboot.
MODID=$(sed -n 's/^id=//p' "$MODPATH/module.prop" 2>/dev/null | head -1)
[ -n "$MODID" ] || MODID=unfuse_zygisk

# --- module config -----------------------------------------------------------
#
# The setting (key path) ships inside the module and is read from there by
# storage.sh and path-mode.sh. The module directory is rewritten whole on every
# install, so the shipped default would wipe the user's choice: the mode is
# carried over from the previous install, and once from the old
# /data/adb/unfuse_zygisk.conf.
#
CONF="$MODPATH/unfuse_zygisk.conf"

# Mode found in a previous install: the module directory first, then the legacy
# /data/adb location.
prev_mode() {
    for f in "/data/adb/modules/$MODID/unfuse_zygisk.conf" \
             /data/adb/unfuse_zygisk.conf; do
        [ -r "$f" ] || continue
        m=$(sed -n 's/^[[:space:]]*path[[:space:]]*=[[:space:]]*//p' "$f" 2>/dev/null \
            | sed 's/[[:space:]]*#.*$//' | sed 's/[[:space:]]*$//' | tail -n 1)
        case "$m" in
            auto|sdcardfs|acl|fuse) printf '%s\n' "$m"; return 0 ;;
            raw)                    printf 'acl\n';     return 0 ;;
        esac
    done
    return 1
}

if [ -f "$CONF" ] && prev="$(prev_mode)"; then
    sed -i "s|^[[:space:]]*path[[:space:]]*=.*|path=$prev|" "$CONF" 2>/dev/null
fi

# The legacy copy is no longer read by anything.
rm -f /data/adb/unfuse_zygisk.conf 2>/dev/null

# What was chosen in the end: the config value, or auto if there is none.
MODE=""
if [ -r "$CONF" ]; then
    MODE=$(sed -n 's/^[[:space:]]*path[[:space:]]*=[[:space:]]*//p' "$CONF" 2>/dev/null \
        | sed 's/[[:space:]]*#.*$//' | sed 's/[[:space:]]*$//' | tail -n 1)
fi
case "$MODE" in
    auto|sdcardfs|acl|fuse) ;;
    raw) MODE=acl ;;
    *) MODE=auto ;;
esac

# --- one line of output ------------------------------------------------------
#
# The installer says exactly one thing: whether the kernel has sdcardfs and which
# path that leaves the module using. Everything else it could report (what the
# module does, how to change the mode, how to disable it) is in the module's own
# WebUI and in path-mode.sh, and printing it here only buries the one fact that
# cannot be read anywhere else at install time.
if grep -qw sdcardfs /proc/filesystems 2>/dev/null; then
    HAVE_SDCARDFS=1
else
    HAVE_SDCARDFS=0
fi

case "$MODE" in
    acl)
        STORAGE="raw /data/media + ACL (mode=acl)" ;;
    fuse)
        # The mode whose whole point is that sdcardfs is irrelevant: vold's
        # FUSE mount is turned into a bind of the raw tree, so the kernel's
        # sdcardfs support does not change the answer.
        STORAGE="raw /data/media, FUSE off (mode=fuse)" ;;
    sdcardfs)
        if [ "$HAVE_SDCARDFS" = 1 ]; then
            STORAGE="sdcardfs (mode=sdcardfs, kernel has sdcardfs)"
        else
            STORAGE="none - mode=sdcardfs but the kernel has no sdcardfs"
        fi ;;
    *)
        if [ "$HAVE_SDCARDFS" = 1 ]; then
            STORAGE="sdcardfs (auto, kernel has sdcardfs)"
        else
            STORAGE="raw /data/media + ACL (auto, kernel has no sdcardfs)"
        fi ;;
esac

ui_print "- Storage path: $STORAGE"
