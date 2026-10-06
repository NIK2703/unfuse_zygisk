#!/system/bin/sh
echo "===== активные строки config ====="
grep '^!' /data/adb/modules/sdcardfs_restore/config 2>&1

echo
echo "===== relax в логе модуля ====="
grep -a "relax:" /data/adb/sdcardfs_restore.log 2>&1 | tail -10

echo
echo "===== права ====="
for p in /mnt/runtime /mnt/runtime/full /mnt/runtime/full/emulated \
         /data/media /data/media/0 /mnt/user /mnt/user/0 /mnt/user/0/emulated; do
    stat -c '%n mode=%a uid=%u gid=%g' "$p" 2>&1
done

echo
echo "===== точки sdcardfs ====="
grep -E ' /mnt/runtime| /mnt/user/0| /storage' /proc/mounts 2>&1 | head -12

echo
echo "===== logcat SdcardFsRestore ====="
logcat -d -s SdcardFsRestore 2>/dev/null | tail -60

echo
echo "===== конец ====="
