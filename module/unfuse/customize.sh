# shellcheck shell=sh
# customize.sh — install stage (сборка unfuse).
#
# Общий шаг установщика (права на дерево, уборка мёртвого конфига) и проверка
# zygisk-плеча лежат в module/common/lib.sh — том же файле, что уходит в архив и
# подключается обеими сборками.

SKIPUNZIP=0

. "$MODPATH/lib.sh"

case "$ARCH" in
    arm64) ABI=arm64-v8a;   FIX=storage-fix-arm64; NOACL=vold-noacl-arm64; FUSEFS=vold-fusefs-arm64 ;;
    arm)   ABI=armeabi-v7a; FIX=storage-fix-arm;   NOACL=vold-noacl-arm;   FUSEFS=vold-fusefs-arm   ;;
    *)     abort "! Unsupported architecture: $ARCH (arm64-v8a or armeabi-v7a required)" ;;
esac

unfuse_require_zygisk_so "$ABI"

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

unfuse_install_common "$ABI"

# .so вне /system: контекст от менеджера не гарантирован — у всех zygisk-модулей на
# устройстве он system_lib_file. Фиксируем явно.
chcon u:object_r:system_lib_file:s0 "$MODPATH/zygisk/$ABI.so" 2>/dev/null || true

# Права только у этой сборки: утилиты vold-плеча.
set_perm "$MODPATH/tools/storage-fix"  0 0 0755
set_perm "$MODPATH/tools/vold-noacl"   0 0 0755
set_perm "$MODPATH/tools/vold-fusefs"  0 0 0755

# Второй конфиг — тот же мёртвый, но лежал внутри модуля.
rm -f "$MODPATH/unfuse_zygisk.conf" 2>/dev/null

# Форму хранилища на 11 модуль выставляет сам (post-fs-data.sh), но смена
# бэкенда не должна быть неожиданной. FUSE — значение AOSP по умолчанию
# (init.rc:792), вендор мог его переопределить. external_storage.sdcardfs.enabled
# приводится к 0 независимо от него: иначе живые чтения vold идут по sdcardfs.
if unfuse_is_android_11; then
    if [ "$(getprop persist.sys.fuse)" = "true" ]; then
        ui_print "- Android 11: FUSE-форма уже активна"
    else
        ui_print "- Android 11: FUSE выключен; модуль включит его при загрузке"
        ui_print "  persist.sys.fuse=true — значение AOSP по умолчанию (init.rc:792)."
    fi
    ui_print "  external_storage.sdcardfs.enabled -> 0"
fi

ui_print "- Storage path: raw /data/media, FUSE off in vold"
