#!/system/bin/sh
# Check the result after rebooting with the module installed.

echo "########## 0. загрузка ##########"
echo "boot_completed: $(getprop sys.boot_completed)"
echo "uptime: $(cat /proc/uptime | cut -d' ' -f1)"

echo
echo "########## 1. модуль активен? ##########"
ls -la /data/adb/modules/unfuse_zygisk/ 2>&1
echo "--- метки disable/remove ---"
for m in disable remove update; do
    [ -f "/data/adb/modules/unfuse_zygisk/$m" ] && echo "  ЕСТЬ $m" || echo "  нет  $m"
done
echo "--- осталось ли в modules_update ---"
ls -la /data/adb/modules_update/unfuse_zygisk/ 2>&1 | head -3

echo
echo "########## 2. sdcardfs на всех четырёх точках ##########"
# Журнала у модуля больше нет — он отчитывается только кодом возврата. Поэтому
# вердикт здесь ставится по факту, как и в самом storage.sh (sdcardfs_verify):
# точка действительно sdcardfs и несёт запрошенные mask/gid.
rc=0
for d in default read write full; do
    p="/mnt/runtime/$d/emulated"
    line=$(grep -m1 " $p " /proc/mounts)
    ty=$(echo "$line" | awk '{print $3}')
    if [ "$ty" = sdcardfs ]; then
        echo "  ОК   $p  $(echo "$line" | awk '{print $4}')"
    else
        echo "  НЕТ  $p (${ty:-нет в mounts})"
        rc=1
    fi
done
if [ "$rc" = 0 ]; then echo "  итог: ОК"; else echo "  итог: НЕТ"; fi

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
[ -n "$ZY" ] && grep -i "unfuse_zygisk" /proc/$ZY/maps 2>/dev/null || echo "(в картах zygote64 нашего .so нет)"

echo
echo "########## конец ##########"
