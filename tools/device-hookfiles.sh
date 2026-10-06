#!/system/bin/sh
# Инвентаризация файлов, относящихся к Zygisk-хукам, на устройстве.
# Ничего не меняет — только читает.

echo "########## 1. модули /data/adb/modules ##########"
ls -la /data/adb/modules 2>/dev/null

echo
echo "########## 2. zygisksu ##########"
ls -la /data/adb/modules/zygisksu 2>/dev/null
echo "--- содержимое рекурсивно ---"
find /data/adb/modules/zygisksu -maxdepth 3 2>/dev/null | head -60

echo
echo "########## 3. zygiskd / libzygisk ##########"
for p in /data/adb/zygisksu /data/adb/zygisk /debug_ramdisk/zygiskd \
         /data/adb/modules/zygisksu/zygiskd /data/adb/modules/zygisksu/libzygisk.so ; do
    if [ -e "$p" ]; then
        echo "--- $p ---"
        ls -la "$p" 2>/dev/null
    fi
done

echo "--- поиск zygisk* по /data/adb (maxdepth 4) ---"
find /data/adb -maxdepth 4 -iname '*zygisk*' 2>/dev/null | head -40

echo
echo "########## 4. процессы ##########"
ps -A -o PID,USER,NAME 2>/dev/null | grep -i -E 'zygisk|zygote' | head -20

echo
echo "########## 5. ksu / apd ##########"
ls -la /data/adb/ksu 2>/dev/null | head -20
for p in /data/adb/ksud /data/adb/ksu/bin/ksud /data/adb/apd ; do
    [ -e "$p" ] && { echo "--- $p ---"; ls -la "$p"; }
done

echo
echo "########## 6. карты памяти zygote (что подгружено) ##########"
ZY=$(pidof zygote64 2>/dev/null)
echo "zygote64 pid: ${ZY:-нет}"
if [ -n "$ZY" ]; then
    grep -i zygisk /proc/$ZY/maps 2>/dev/null | head -20
    echo "--- NSpid/mount ns ---"
    ls -l /proc/$ZY/ns/mnt 2>/dev/null
fi

echo
echo "########## 7. свойства ##########"
getprop 2>/dev/null | grep -i -E 'zygisk|kernelsu|ksu' | head -20

echo
echo "########## 8. версия ZygiskNext из module.prop ##########"
for f in /data/adb/modules/zygisksu/module.prop ; do
    [ -f "$f" ] && { echo "--- $f ---"; cat "$f"; }
done

echo
echo "########## конец ##########"
