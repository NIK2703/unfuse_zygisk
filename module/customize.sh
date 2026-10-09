# shellcheck shell=sh
# customize.sh — install stage.

SKIPUNZIP=0

case "$ARCH" in
    arm64) ABI=arm64-v8a;   FIX=storage-fix-arm64; NOACL=vold-noacl-arm64; FUSEFS=vold-fusefs-arm64 ;;
    arm)   ABI=armeabi-v7a; FIX=storage-fix-arm;   NOACL=vold-noacl-arm;   FUSEFS=vold-fusefs-arm   ;;
    *)     abort "! Unsupported architecture: $ARCH (arm64-v8a or armeabi-v7a required)" ;;
esac

if [ ! -f "$MODPATH/zygisk/$ABI.so" ]; then
    abort "! zygisk/$ABI.so is missing - the build did not run (build.sh)"
fi

# Every ABI's tools are in the archive; keep ours under the plain names.
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
set_perm "$MODPATH/description.txt" 0 0 0644 2>/dev/null

# Leftovers of the old config; nothing reads them.
rm -f /data/adb/unfuse_zygisk.conf 2>/dev/null
rm -f "$MODPATH/unfuse_zygisk.conf" 2>/dev/null

# Форму хранилища на 11 модуль выставляет сам (post-fs-data.sh), но смена
# бэкенда не должна быть неожиданной. FUSE — значение AOSP по умолчанию
# (init.rc:792), вендор мог его переопределить. external_storage.sdcardfs.enabled
# приводится к 0 независимо от него: иначе живые чтения vold идут по sdcardfs.
if [ "$(getprop ro.build.version.sdk)" = "30" ]; then
    if [ "$(getprop persist.sys.fuse)" = "true" ]; then
        ui_print "- Android 11: FUSE-форма уже активна"
    else
        ui_print "- Android 11: FUSE выключен; модуль включит его при загрузке"
        ui_print "  persist.sys.fuse=true — значение AOSP по умолчанию (init.rc:792)."
    fi
    ui_print "  external_storage.sdcardfs.enabled -> 0"
fi

ui_print "- Storage path: raw /data/media, FUSE off in vold"
