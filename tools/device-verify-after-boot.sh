#!/system/bin/sh
# Проверка результата после перезагрузки с установленным модулем.

echo "########## 0. загрузка ##########"
echo "boot_completed: $(getprop sys.boot_completed)"
echo "uptime: $(cat /proc/uptime | cut -d' ' -f1)"

echo
echo "########## 1. модуль активен? ##########"
ls -la /data/adb/modules/sdcardfs_restore/ 2>&1
echo "--- метки disable/remove ---"
for m in disable remove update; do
    [ -f "/data/adb/modules/sdcardfs_restore/$m" ] && echo "  ЕСТЬ $m" || echo "  нет  $m"
done
echo "--- осталось ли в modules_update ---"
ls -la /data/adb/modules_update/sdcardfs_restore/ 2>&1 | head -3

echo
echo "########## 2. лог модуля ##########"
if [ -f /data/adb/sdcardfs_restore.log ]; then
    echo "размер: $(wc -c < /data/adb/sdcardfs_restore.log) байт"
    cat /data/adb/sdcardfs_restore.log
else
    echo "ЛОГА НЕТ — post-fs-data.sh/service.sh не отработали!"
fi

echo
echo "########## 3. sdcardfs на /mnt/runtime/*/emulated ##########"
grep -E "mnt/runtime" /proc/mounts 2>&1
echo "--- типы ---"
for d in default read write full; do
    p="/mnt/runtime/$d/emulated"
    t=$(stat -f "$p" 2>/dev/null | grep -o "Type:.*")
    echo "  $p -> $t"
done

echo
echo "########## 4. видит ли ZygiskNext наш модуль ##########"
/data/adb/modules/zygisksu/bin/zygiskd status 2>&1

echo
echo "########## 5. наш .so в zygote? ##########"
ZY=$(pidof zygote64 2>/dev/null)
echo "zygote64 pid: ${ZY:-нет}"
[ -n "$ZY" ] && grep -i "sdcardfs_restore" /proc/$ZY/maps 2>/dev/null || echo "(в картах zygote64 нашего .so нет)"

echo
echo "########## 6. logcat про наш модуль ##########"
logcat -d -s SdcardFsRestore 2>/dev/null | tail -40

echo
echo "########## конец ##########"
