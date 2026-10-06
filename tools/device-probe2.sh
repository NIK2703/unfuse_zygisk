#!/system/bin/sh
# Второй этап разведки: почему sdcardfs выключен и есть ли всё нужное.

echo "===== A. /system/bin/sdcard ====="
ls -la /system/bin/sdcard 2>&1
ls -la /system/bin/sdcard* 2>&1
echo "--- есть ли строка sdcard в init/vold rc ---"
grep -rl 'bin/sdcard' /system/etc/init /vendor/etc/init 2>/dev/null

echo
echo "===== B. ОТКУДА external_storage.sdcardfs.enabled=0 ====="
for f in /system/build.prop /system/etc/prop.default /vendor/build.prop /vendor/default.prop \
         /odm/etc/build.prop /product/build.prop /system_ext/build.prop /data/local.prop \
         /data/property/persistent_properties; do
    if [ -f "$f" ]; then
        r=$(grep -a 'sdcardfs' "$f" 2>/dev/null)
        if [ -n "$r" ]; then
            echo ">>> $f"
            echo "$r"
        fi
    fi
done
echo "--- resetprop-совместимые дампы ---"
grep -a 'external_storage' /data/property/persistent_properties 2>/dev/null | head -5
echo "--- getprop --verbose? ---"
getprop external_storage.sdcardfs.enabled

echo
echo "===== C. ZYGISK ====="
echo "--- модули (с признаком disable) ---"
for m in /data/adb/modules/*; do
    [ -d "$m" ] || continue
    n=$(basename "$m")
    flag=""
    [ -f "$m/disable" ] && flag="$flag disabled"
    [ -f "$m/remove" ] && flag="$flag remove"
    [ -d "$m/zygisk" ] && flag="$flag ZYGISK($(ls "$m/zygisk" 2>/dev/null | tr '\n' ',' ))"
    echo "$n$flag"
done
echo "--- zygisknext / ksud ---"
ls -la /data/adb/zygisksu 2>&1 | head
ls -la /data/adb/ksu 2>&1 | head
ls -la /data/adb/ksud 2>&1
find /data/adb -maxdepth 3 -iname '*zygisk*' 2>/dev/null | head -20
echo "--- процесс zygote / zygiskd ---"
ps -A -o USER,PID,NAME 2>/dev/null | grep -iE 'zygisk|zygote' | head -10

echo
echo "===== D. СЛОИ: где лежит vold ====="
ls -la /system/bin/vold 2>&1
ls -la /system/bin/vold_prepare_subdirs 2>&1
echo "--- монтирования /system ---"
grep -E ' /system | /vendor | /product | /system_ext ' /proc/mounts

echo
echo "===== E. ПРАВА НА /data/media ====="
ls -lad /data/media /data/media/0 2>&1
stat -c '%A %U %G %n' /data/media /data/media/0 2>&1

echo
echo "===== F. ПРОБА ЗАПУСКА /system/bin/sdcard (dry) ====="
/system/bin/sdcard 2>&1 | head -5

echo
echo "===== КОНЕЦ ====="
