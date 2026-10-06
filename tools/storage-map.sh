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
echo "--- переключатель альтернативного пути ---"
if [ -e /data/adb/sdcardfs_restore.force_raw ]; then
    echo "  force_raw НА МЕСТЕ — модуль принудительно на альтернативном пути"
else
    echo "  force_raw нет — модуль на основном пути"
fi
