# shellcheck shell=sh
#
# customize.sh — вызывается установщиком Magisk/KernelSU на этапе распаковки модуля.
# Задача: проверить, что сборка на месте, и выставить права.
#

SKIPUNZIP=0

ui_print " "
ui_print "*********************************************"
ui_print "  sdcardfs Restore"
ui_print "*********************************************"
ui_print " "

# ------------------------------------------------------------------ ABI
case "$ARCH" in
    arm64)
        ABI=arm64-v8a
        ;;
    arm)
        ABI=armeabi-v7a
        ;;
    *)
        ui_print "! Неподдерживаемая архитектура: $ARCH"
        ui_print "! Модуль собран только под arm64-v8a и armeabi-v7a."
        abort "  Установка прервана."
        ;;
esac

if [ ! -f "$MODPATH/zygisk/$ABI.so" ]; then
    ui_print "! В модуле нет zygisk/$ABI.so"
    abort "  Похоже, сборка не выполнялась. Запустите build.sh."
fi
ui_print "- Zygisk-библиотека: zygisk/$ABI.so"

# ------------------------------------------------------------------ API
if [ -n "$API" ] && [ "$API" -lt 29 ] 2>/dev/null; then
    ui_print "! Android API $API — модуль рассчитан на Android 11+"
fi

# ------------------------------------------------------------------ конфиг
# Конфиг лежит в каталоге модуля намеренно: /data/adb/*.conf помечен
# adb_data_file, а домен zygote не имеет права читать data_file_type
# (neverhard в AOSP zygote.te). Каталог модуля перемаркирован в system_file
# и читается без проблем.
CONF="$MODPATH/config"

if [ ! -f "$CONF" ]; then
    ui_print "! В модуле нет config — создаю минимальный"
    printf '# см. комментарии в оригинале\n!enabled=1\n!android_dirs=raw\n' > "$CONF"
fi

# ------------------------------------------------------------------ права
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/zygisk/$ABI.so" 0 0 0644
set_perm "$MODPATH/customize.sh" 0 0 0755 2>/dev/null
set_perm "$MODPATH/service.sh" 0 0 0755 2>/dev/null
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755 2>/dev/null
set_perm "$MODPATH/relabel-media.sh" 0 0 0755 2>/dev/null
set_perm "$MODPATH/relax-storage.sh" 0 0 0755 2>/dev/null
set_perm "$MODPATH/sdcardfs-bringup.sh" 0 0 0755 2>/dev/null

# ------------------------------------------------------------------ напоминание
ui_print " "
ui_print "- Модуль работает для ВСЕХ приложений без исключений:"
ui_print "  внутренняя память отдаётся через sdcardfs с полным доступом,"
ui_print "  включая Android/data, Android/obb и каталоги чужих пакетов —"
ui_print "  как на Android 10 и более ранних версиях."
ui_print " "
ui_print "- Настройки (не список приложений, а только глобальные флаги):"
ui_print "    $CONF"
ui_print "  Например !enabled=0 чтобы выключить, !verbose=1 для подробного лога."
ui_print " "
ui_print "- Аварийное отключение без правки конфига:"
ui_print "    touch /data/adb/sdcardfs_restore.disable"
ui_print " "
ui_print "- Для работы требуется:"
ui_print "    1) ядро с CONFIG_SDCARD_FS=y"
ui_print "    2) включённый Zygisk (Magisk) или ZygiskNext (KernelSU/APatch)"
ui_print " "
ui_print "- Модуль сам поднимает sdcardfs на /mnt/runtime/*/emulated, чтобы"
ui_print "  обойтись дешёвым bind; если это не удастся — он смонтирует sdcardfs"
ui_print "  самостоятельно, прямо в namespace приложения."
ui_print " "
if [ -f "$MODPATH/relabel-media.sh" ]; then
    ui_print "- relabel-media.sh: перемаркирует корень /data/media из"
    ui_print "  media_userdir_file в media_rw_data_file. Без этого stat() и"
    ui_print "  list() самого /storage/emulated отдают EACCES: sdcardfs"
    ui_print "  форвардит getattr корня в нижний инод, а у media_userdir_file"
    ui_print "  для приложений разрешён только search. Содержимое"
    ui_print "  (/storage/emulated/0 и глубже) работает и без этого."
else
    ui_print "! relabel-media.sh отсутствует — корень /storage/emulated будет"
    ui_print "  отдавать EACCES на stat()/list()."
fi
ui_print " "
