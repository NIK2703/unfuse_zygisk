#!/system/bin/sh
#
# dev-boot-journal.sh — что модуль сделал за эту загрузку.
#
# Запуск: su -c 'sh /data/local/tmp/dev-boot-journal.sh'

LOG=/data/adb/sdcardfs_restore.log
CONF=/data/adb/sdcardfs_restore.conf
LEGACY=/data/adb/sdcardfs_restore.force_raw

echo "=== uptime ==="
uptime

echo
echo "=== какой модуль стоит (после перезагрузки) ==="
ls -la /data/adb/modules/sdcardfs_restore/ 2>&1
echo "--- остался ли каталог modules_update ---"
ls -d /data/adb/modules_update/sdcardfs_restore 2>&1

echo
echo "=== настройка ==="
echo "конфиг: $CONF"
if [ -r "$CONF" ]; then
    grep -v '^[[:space:]]*#' "$CONF" | grep -v '^[[:space:]]*$' | sed 's/^/  /'
else
    echo "  НЕТ"
fi
if [ -e "$LEGACY" ]; then
    echo "метка force_raw: на месте"
else
    echo "метка force_raw: нет"
fi

echo
echo "=== строки о режиме пути за последнюю загрузку ==="
grep -a 'режим пути:' "$LOG" | tail -n 6 | sed 's/^/  /'

echo
echo "=== патч vold ==="
grep -aE 'vold-noacl|патч vold' "$LOG" | tail -n 8 | sed 's/^/  /'

echo
echo "=== путь, которым пошёл модуль ==="
grep -aE 'основной путь|альтернативный путь|ACL /data/media/0' "$LOG" \
    | tail -n 10 | sed 's/^/  /'

echo
echo "=== что стоит на точках ==="
for p in /mnt/runtime/default/emulated /mnt/runtime/read/emulated \
         /mnt/runtime/write/emulated   /mnt/runtime/full/emulated; do
    t=""
    while read -r _dev mp ty _rest; do
        [ "$mp" = "$p" ] && t="$ty"
    done < /proc/mounts
    printf '  %-32s %s\n' "$p" "${t:-—}"
done

echo
echo "=== default-ACL у /data/media/0 (должна быть 9997, не 1023) ==="
/data/local/tmp/acl-dump /data/media/0 2>&1 | sed 's/^/  /'
