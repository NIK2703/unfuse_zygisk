#!/system/bin/sh
# storage-map.sh — which filesystem backs external storage: the module's path is
# sdcardfs on /mnt/runtime/*/emulated, and there is no alternative.

echo "--- /proc/filesystems: есть ли sdcardfs в ядре ---"
if grep -qw sdcardfs /proc/filesystems; then
    echo "  да: $(grep -w sdcardfs /proc/filesystems)"
else
    echo "  нет — модуль на этом ядре не ставится"
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
echo "--- что говорит журнал модуля ---"
if [ -r /data/adb/unfuse_zygisk.log ]; then
    grep -a 'основной путь' /data/adb/unfuse_zygisk.log | tail -n 4 | sed 's/^/  /'
    [ -n "$(grep -a 'основной путь' /data/adb/unfuse_zygisk.log)" ] || \
        echo "  (строк «основной путь» нет — журнал от старой сборки)"
else
    echo "  журнала нет"
fi
