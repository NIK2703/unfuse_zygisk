# shellcheck shell=sh
#
# customize.sh — вызывается установщиком Magisk/KernelSU при распаковке модуля:
# проверяет, что сборка на месте, оставляет бинарник утилиты под свою
# архитектуру и выставляет права.
#

SKIPUNZIP=0

case "$ARCH" in
    arm64) ABI=arm64-v8a;   FIX=storage-fix-arm64; NOACL=vold-noacl-arm64 ;;
    arm)   ABI=armeabi-v7a; FIX=storage-fix-arm;   NOACL=vold-noacl-arm   ;;
    *)     abort "! Неподдерживаемая архитектура: $ARCH (нужны arm64-v8a или armeabi-v7a)" ;;
esac

if [ ! -f "$MODPATH/zygisk/$ABI.so" ]; then
    abort "! В модуле нет zygisk/$ABI.so — похоже, сборка не выполнялась (build.sh)"
fi

# В архиве лежат бинарники обеих утилит на все ABI: оставляем только свои и
# переименовываем — под этими именами их ищут storage.sh, post-fs-data.sh и
# service.sh.
if [ ! -f "$MODPATH/tools/$FIX" ]; then
    abort "! В модуле нет tools/$FIX — похоже, сборка не выполнялась (build.sh)"
fi
if [ ! -f "$MODPATH/tools/$NOACL" ]; then
    abort "! В модуле нет tools/$NOACL — похоже, сборка не выполнялась (build.sh)"
fi
mv "$MODPATH/tools/$FIX"   "$MODPATH/tools/storage-fix"
mv "$MODPATH/tools/$NOACL" "$MODPATH/tools/vold-noacl"
rm -f "$MODPATH/tools/storage-fix-arm64" "$MODPATH/tools/storage-fix-arm" \
      "$MODPATH/tools/vold-noacl-arm64"  "$MODPATH/tools/vold-noacl-arm"

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/zygisk/$ABI.so"     0 0 0644
set_perm "$MODPATH/tools/storage-fix"  0 0 0755
set_perm "$MODPATH/tools/vold-noacl"   0 0 0755
set_perm "$MODPATH/customize.sh"       0 0 0755 2>/dev/null
set_perm "$MODPATH/post-fs-data.sh"    0 0 0755 2>/dev/null
set_perm "$MODPATH/service.sh"         0 0 0755 2>/dev/null
set_perm "$MODPATH/storage.sh"         0 0 0755 2>/dev/null
set_perm "$MODPATH/path-mode.sh"       0 0 0755 2>/dev/null

# --- конфиг модуля -----------------------------------------------------------
#
# Настройка модуля (ключ path) живёт в /data/adb/sdcardfs_restore.conf, рядом с
# журналом, а НЕ в каталоге модуля: тот при каждой установке перезаписывается
# целиком, и настройка молча пропадала бы при обновлении. Из архива конфиг
# копируется ровно один раз — если файла ещё нет. Поэтому режим, выставленный
# пользователем, переживает и обновление модуля, и переустановку.
#
CONF=/data/adb/sdcardfs_restore.conf
if [ -f "$MODPATH/sdcardfs_restore.conf" ] && [ ! -e "$CONF" ]; then
    cp -f "$MODPATH/sdcardfs_restore.conf" "$CONF" 2>/dev/null \
        && chmod 0644 "$CONF" 2>/dev/null
fi

# Что в итоге выбрано: значение конфига, а если его нет — auto.
MODE=""
if [ -r "$CONF" ]; then
    MODE=$(sed -n 's/^[[:space:]]*path[[:space:]]*=[[:space:]]*//p' "$CONF" 2>/dev/null \
        | sed 's/[[:space:]]*#.*$//' | sed 's/[[:space:]]*$//' | tail -n 1)
fi
case "$MODE" in
    auto|sdcardfs|raw) ;;
    *) MODE=auto ;;
esac

ui_print "- Внутренняя память отдаётся приложениям напрямую:"
ui_print "  корень /storage/emulated, Android/data, Android/obb и каталоги"
ui_print "  чужих пакетов — как на Android 10 и раньше."

case "$MODE" in
    raw)
        ui_print "- Режим пути: raw — только сырое дерево /data/media с ACL на"
        ui_print "  группу 9997 (AID_EVERYBODY); sdcardfs не поднимается."
        ;;
    sdcardfs)
        ui_print "- Режим пути: sdcardfs — только основной путь. Если ядро его не"
        ui_print "  умеет, приложения останутся без памяти (альтернативный запрещён)."
        ;;
    *)
        if grep -qw sdcardfs /proc/filesystems 2>/dev/null; then
            ui_print "- Режим пути: auto — ядро умеет sdcardfs, будет основной путь."
        else
            ui_print "- Режим пути: auto — в ядре нет sdcardfs, включится"
            ui_print "  альтернативный: сырое дерево /data/media с ACL на группу 9997."
        fi
        ;;
esac

# Идентификатор модуля — чтобы подсказка указывала на путь ПОСЛЕ перезагрузки, а
# не на каталог установки: $MODPATH на стадии установки смотрит в
# /data/adb/modules_update/, который до перезагрузки не работает.
MODID=$(sed -n 's/^id=//p' "$MODPATH/module.prop" 2>/dev/null | head -1)
[ -n "$MODID" ] || MODID=sdcardfs_restore

ui_print "- Настройка модуля: $CONF (ключ path). Сменить —"
ui_print "  sh /data/adb/modules/$MODID/path-mode.sh raw|sdcardfs|auto"
ui_print "- Отключить: создать /data/adb/modules/$MODID/disable и перезагрузиться."
