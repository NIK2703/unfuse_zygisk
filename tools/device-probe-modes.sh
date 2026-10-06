#!/system/bin/sh
echo "===== режимы и владельцы ====="
for p in / /mnt /mnt/runtime /mnt/runtime/full /mnt/runtime/full/emulated \
         /mnt/runtime/full/emulated/0 /mnt/user /mnt/user/0 /mnt/user/0/emulated \
         /data /data/media /data/media/0; do
    stat -c '%n mode=%a uid=%u gid=%g ctx=%C' "$p" 2>&1
done

echo
echo "===== метки глубже по sdcardfs ====="
for p in /mnt/runtime/full/emulated/0 /mnt/runtime/full/emulated/0/Download \
         /mnt/runtime/full/emulated/obb; do
    ls -Zd "$p" 2>&1
done

echo
echo "===== shell (uid 2000) видит? ====="
id
stat -c '%n mode=%a' /mnt/runtime 2>&1
stat -c '%n mode=%a' /mnt/runtime/full/emulated 2>&1

echo
echo "===== все недавние AVC (logcat) ====="
logcat -d -b all 2>/dev/null | grep -i "avc:" | tail -25

echo
echo "===== конец ====="
