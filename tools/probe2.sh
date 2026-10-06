#!/system/bin/sh
# Probe 2: why sdcardfs is not mounted.

echo "===== probe2: $(date) ====="

echo
echo "--- все свойства со storage/sdcardfs/fuse ---"
getprop | grep -iE 'sdcardfs|fuse|storage' | head -40

echo
echo "--- откуда берётся external_storage.sdcardfs.enabled ---"
for f in /system/build.prop /vendor/build.prop /odm/etc/build.prop \
         /vendor/odm/etc/build.prop /system/system/build.prop \
         /system_ext/build.prop /product/build.prop /my_product/build.prop \
         /my_product/etc/build.prop; do
    if [ -f "$f" ]; then
        hit=$(grep -n 'sdcardfs' "$f" 2>/dev/null)
        if [ -n "$hit" ]; then
            echo "  $f:"
            echo "$hit" | sed 's/^/      /'
        fi
    fi
done
echo "  (если пусто — свойства в build.prop нет)"

echo
echo "--- поиск по всем prop-файлам (может занять пару секунд) ---"
grep -rl 'sdcardfs' /system /vendor /odm /product /system_ext 2>/dev/null | head -20

echo
echo "--- модули, которые трогают storage/sdcardfs ---"
for d in /data/adb/modules/*/; do
    n=$(basename "$d")
    hit=$(grep -rls 'sdcardfs\|external_storage' "$d" 2>/dev/null | head -5)
    if [ -n "$hit" ]; then
        echo "  [$n]"
        echo "$hit" | sed 's/^/      /'
    fi
done

echo
echo "--- system.prop модулей ---"
for f in /data/adb/modules/*/system.prop; do
    [ -f "$f" ] || continue
    echo "  $f:"
    sed 's/^/      /' "$f"
done

echo
echo "--- post-fs-data.sh модулей (первые строки) ---"
for f in /data/adb/modules/*/post-fs-data.sh; do
    [ -f "$f" ] || continue
    echo "  === $f ==="
    head -25 "$f" | sed 's/^/      /'
done

echo
echo "--- sdcardfs в kallsyms ---"
if [ -r /proc/kallsyms ]; then
    echo -n "  всего символов sdcardfs: "
    grep -c sdcardfs /proc/kallsyms
    echo "  примеры:"
    grep sdcardfs /proc/kallsyms | head -12 | sed 's/^/      /'
    echo -n "  kptr_restrict="; cat /proc/sys/kernel/kptr_restrict 2>/dev/null
else
    echo "  /proc/kallsyms недоступен"
fi

echo
echo "--- dmesg про sdcardfs ---"
dmesg 2>/dev/null | grep -i sdcardfs | tail -20 || echo "  (dmesg недоступен)"

echo
echo "--- vold: что в логе про sdcardfs / emulated ---"
logcat -d -b all 2>/dev/null | grep -iE 'sdcardfs' | tail -20 || echo "  (нет)"

echo
echo "--- zygisk ---"
ls -la /data/adb/modules/zygisksu/ 2>&1 | head
echo "  zygisk-модули (у кого есть каталог zygisk/):"
for d in /data/adb/modules/*/; do
    [ -d "$d/zygisk" ] && echo "      $(basename $d): $(ls $d/zygisk)"
done

echo
echo "--- /proc/filesystems полностью ---"
cat /proc/filesystems

echo
echo "===== конец ====="
