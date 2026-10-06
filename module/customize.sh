# shellcheck shell=sh
#
# customize.sh — вызывается установщиком Magisk/KernelSU при распаковке модуля:
# проверяет, что сборка на месте, оставляет бинарник утилиты под свою
# архитектуру и выставляет права.
#

SKIPUNZIP=0

case "$ARCH" in
    arm64) ABI=arm64-v8a;   FIX=storage-fix-arm64 ;;
    arm)   ABI=armeabi-v7a; FIX=storage-fix-arm ;;
    *)     abort "! Неподдерживаемая архитектура: $ARCH (нужны arm64-v8a или armeabi-v7a)" ;;
esac

if [ ! -f "$MODPATH/zygisk/$ABI.so" ]; then
    abort "! В модуле нет zygisk/$ABI.so — похоже, сборка не выполнялась (build.sh)"
fi

# В архиве лежат бинарники утилиты на все ABI: оставляем только свой и
# переименовываем в tools/storage-fix, под которым её ищет storage.sh.
if [ ! -f "$MODPATH/tools/$FIX" ]; then
    abort "! В модуле нет tools/$FIX — похоже, сборка не выполнялась (build.sh)"
fi
mv "$MODPATH/tools/$FIX" "$MODPATH/tools/storage-fix"
rm -f "$MODPATH/tools/storage-fix-arm64" "$MODPATH/tools/storage-fix-arm"

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/zygisk/$ABI.so"    0 0 0644
set_perm "$MODPATH/tools/storage-fix" 0 0 0755
set_perm "$MODPATH/customize.sh"      0 0 0755 2>/dev/null
set_perm "$MODPATH/post-fs-data.sh"   0 0 0755 2>/dev/null
set_perm "$MODPATH/service.sh"        0 0 0755 2>/dev/null
set_perm "$MODPATH/storage.sh"        0 0 0755 2>/dev/null

ui_print "- Внутренняя память отдаётся приложениям напрямую:"
ui_print "  корень /storage/emulated, Android/data, Android/obb и каталоги"
ui_print "  чужих пакетов — как на Android 10 и раньше."

if grep -qw sdcardfs /proc/filesystems 2>/dev/null; then
    ui_print "- Ядро умеет sdcardfs: используется основной путь."
else
    ui_print "- В ядре нет sdcardfs: включится альтернативный путь — сырое"
    ui_print "  дерево /data/media с ACL на группу 9997 (AID_EVERYBODY)."
fi

ui_print "- Отключить: создать /data/adb/modules/sdcardfs_restore/disable и перезагрузиться."
