#!/system/bin/sh
echo "===== метки нижнего слоя ====="
for p in /data/media /data/media/0 /data/media/obb /data/media/0/Android \
         /data/media/0/Android/data /data/media/0/Android/obb /data/media/0/Download; do
    ls -Zd "$p" 2>&1
done

echo
echo "===== режимы ====="
for p in /data/media/obb /data/media/0/Android/data /data/media/0/Android/obb; do
    stat -c '%n mode=%a uid=%u gid=%g' "$p" 2>&1
done

echo
echo "===== можно ли переключить домен через runcon ====="
runcon u:r:untrusted_app:s0 sh -c 'id' 2>&1 | head -3
echo "---"
runcon u:r:untrusted_app:s0 sh -c 'stat -c %a /mnt/runtime/full/emulated' 2>&1 | head -3

echo
echo "===== policy: есть ли media_userdir_file у appdomain ====="
if [ -r /sys/fs/selinux/policy ]; then
    echo "policy читается ($(stat -c %s /sys/fs/selinux/policy) байт)"
else
    echo "policy недоступна"
fi

echo
echo "===== конец ====="
