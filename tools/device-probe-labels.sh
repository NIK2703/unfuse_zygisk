#!/system/bin/sh
# What actually lives in /mnt/runtime/*/emulated and how it is labeled.
echo "===== getenforce ====="
getenforce

echo
echo "===== ls -Zd (метки + тип) ====="
for p in /mnt /mnt/runtime /mnt/runtime/full /mnt/runtime/full/emulated \
         /mnt/runtime/write /mnt/runtime/write/emulated \
         /mnt/user /mnt/user/0 /mnt/user/0/emulated /storage/emulated /data/media; do
    ls -Zd "$p" 2>&1
done

echo
echo "===== stat (метка + права) ====="
for p in /mnt/runtime/full/emulated /mnt/user/0/emulated; do
    echo "--- $p"
    stat -c '%n mode=%a uid=%u gid=%g ctx=%C' "$p" 2>&1
done

echo
echo "===== монтирования /mnt/runtime и /mnt/user/0 ====="
grep -E ' /mnt/runtime| /mnt/user/0| /storage' /proc/mounts 2>&1

echo
echo "===== файловая система в точках ====="
for p in /mnt/runtime/full/emulated /mnt/runtime/write/emulated /mnt/user/0/emulated; do
    echo "--- $p"
    stat -f -c 'type=%t (%T) blocks=%b' "$p" 2>&1
done

echo
echo "===== видно ли содержимое ====="
for p in /mnt/runtime/full/emulated /mnt/user/0/emulated; do
    echo "--- $p"
    ls -la "$p" 2>&1 | head -8
done

echo
echo "===== avc в dmesg ====="
dmesg 2>/dev/null | grep -i "avc" | tail -20

echo
echo "===== конец ====="
