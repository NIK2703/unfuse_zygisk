#!/system/bin/sh
# Can sdcardfs be raised on the stock /mnt/runtime/*/emulated points exactly as vold does? Reversible: all four mounts are unmounted at the end.

echo "===== 0. ДО ====="
for d in default read write full; do
    p=/mnt/runtime/$d/emulated
    echo "--- $p ---"
    stat -c '  %A %a %u %g' "$p" 2>&1
    ls -A "$p" 2>&1 | head -3
    stat -f "$p" 2>&1 | grep Type
done
echo "--- сейчас смонтировано на этих точках ---"
grep -E 'mnt/runtime' /proc/mounts || echo "  (ничего)"

echo
echo "===== 1. ЗАПУСК /system/bin/sdcard (как vold) ====="
/system/bin/sdcard -u 1023 -g 1023 -m -w -G -i -o /data/media emulated
echo "rc=$?"

echo
echo "===== 2. ПОСЛЕ ====="
grep -E 'mnt/runtime' /proc/mounts || echo "  НЕ СМОНТИРОВАНО"

echo
echo "===== 3. СОДЕРЖИМОЕ ====="
for d in default read write full; do
    p=/mnt/runtime/$d/emulated
    echo "--- $p ---"
    stat -f "$p" 2>&1 | grep Type
    stat -c '  %A %a uid=%u gid=%g %n' "$p" "$p/0" 2>&1
    echo "  содержимое /0:"
    ls -A "$p/0" 2>&1 | head -6 | sed 's/^/    /'
done

echo
echo "===== 4. ПРАВА НА FULL ====="
P=/mnt/runtime/full/emulated
for x in "" /0 /0/Download /0/Android /0/Android/data /0/Android/obb; do
    stat -c '  %A %a uid=%u gid=%g %n' "$P$x" 2>&1
done

echo
echo "===== 5. ПРОБА ЗАПИСИ ОТ ПРИЛОЖЕНИЯ ====="
if [ -x /data/local/tmp/runas ]; then
    /data/local/tmp/runas 10465 10465 "10465,20465,9997,3003" \
        touch "$P/0/Download/.probe_bringup" 2>&1 && echo "  WRITE OK" || echo "  WRITE DENY"
    ls -la "$P/0/Download/.probe_bringup" 2>&1
    rm -f "$P/0/Download/.probe_bringup"
fi

echo
echo "===== 6. ОТКАТ: снимаем маунты ====="
for d in default read write full; do
    p=/mnt/runtime/$d/emulated
    umount "$p" && echo "  umounted $p" || echo "  FAIL $p"
done
echo "--- остатки ---"
grep -E 'mnt/runtime' /proc/mounts || echo "  чисто"

echo
echo "===== КОНЕЦ ====="
