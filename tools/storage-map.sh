#!/system/bin/sh
#
# storage-map.sh — какой файловой системой сейчас отдаётся внешнее хранилище.
#
# Запуск:  su -c 'sh /data/local/tmp/storage-map.sh'
#
# Нужен, чтобы отличить основной путь (sdcardfs, поднятый модулем)
# от альтернативного (сырое /data/media плюс ACL на группу 9997).

echo "--- /proc/filesystems: есть ли sdcardfs в ядре ---"
if grep -qw sdcardfs /proc/filesystems; then
    echo "  да: $(grep -w sdcardfs /proc/filesystems)"
else
    echo "  нет"
fi

echo
echo "--- тип ФС на путях внешнего хранилища ---"
for p in /data/media \
         /mnt/runtime/default/emulated \
         /mnt/runtime/read/emulated \
         /mnt/runtime/write/emulated \
         /mnt/runtime/full/emulated \
         /mnt/user/0/emulated \
         /storage/emulated; do
    line=$(grep -m1 " $p " /proc/mounts)
    if [ -n "$line" ]; then
        printf '  %-34s %s\n' "$p" "$(echo "$line" | awk '{print $3}')"
    else
        printf '  %-34s %s\n' "$p" "(нет в mounts)"
    fi
done

echo
echo "--- что лежит под /mnt/runtime/full/emulated (первые записи) ---"
ls -ld /mnt/runtime/full/emulated 2>&1
ls /mnt/runtime/full/emulated 2>&1 | head -8

echo
echo "--- настройка пути модуля: /data/adb/sdcardfs_restore.conf ---"
CONF=/data/adb/sdcardfs_restore.conf
if [ -r "$CONF" ]; then
    mode=$(sed -n 's/^[[:space:]]*path[[:space:]]*=[[:space:]]*//p' "$CONF" 2>/dev/null \
        | sed 's/[[:space:]]*#.*$//' | sed 's/[[:space:]]*$//' | tail -n 1)
    echo "  path=${mode:-<пусто>}  (файл есть)"
else
    echo "  конфига нет — действует значение по умолчанию: auto"
fi

if [ -e /data/adb/sdcardfs_restore.force_raw ]; then
    echo "  устаревшая метка force_raw НА МЕСТЕ — означает path=raw,"
    echo "  но явный path в конфиге главнее (см. журнал модуля)"
fi

echo
echo "--- что говорит журнал модуля о выбранном режиме ---"
if [ -r /data/adb/sdcardfs_restore.log ]; then
    grep -a 'режим пути:' /data/adb/sdcardfs_restore.log | tail -n 4 | sed 's/^/  /'
    [ -n "$(grep -a 'режим пути:' /data/adb/sdcardfs_restore.log)" ] || \
        echo "  (строк «режим пути:» нет — журнал от сборки до v3.2.0)"
else
    echo "  журнала нет"
fi
