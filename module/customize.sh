# shellcheck shell=sh
#
# customize.sh — вызывается установщиком Magisk/KernelSU при распаковке модуля:
# проверяет, что сборка на месте и что ядро умеет sdcardfs, и выставляет права.
#

SKIPUNZIP=0

case "$ARCH" in
    arm64) ABI=arm64-v8a ;;
    arm)   ABI=armeabi-v7a ;;
    *)     abort "! Неподдерживаемая архитектура: $ARCH (нужны arm64-v8a или armeabi-v7a)" ;;
esac

if [ ! -f "$MODPATH/zygisk/$ABI.so" ]; then
    abort "! В модуле нет zygisk/$ABI.so — похоже, сборка не выполнялась (build.sh)"
fi

if ! grep -qw sdcardfs /proc/filesystems 2>/dev/null; then
    ui_print "! В ядре нет sdcardfs (/proc/filesystems) — нужно CONFIG_SDCARD_FS,"
    ui_print "  без него модуль работать не сможет."
fi

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/zygisk/$ABI.so"  0 0 0644
set_perm "$MODPATH/customize.sh"    0 0 0755 2>/dev/null
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755 2>/dev/null
set_perm "$MODPATH/service.sh"      0 0 0755 2>/dev/null
set_perm "$MODPATH/storage.sh"      0 0 0755 2>/dev/null

ui_print "- Внутренняя память отдаётся через sdcardfs для ВСЕХ приложений:"
ui_print "  корень /storage/emulated, Android/data, Android/obb и каталоги"
ui_print "  чужих пакетов — как на Android 10 и раньше."
ui_print "- Отключить: создать /data/adb/modules/sdcardfs_restore/disable и перезагрузиться."
