#!/system/bin/sh
# Prerequisites for Unfuse Zygisk. Read-only.

echo "===== 1. ЯДРО И ДРАЙВЕР ====="
echo "--- uname ---"
uname -a
echo "--- sdcardfs в /proc/filesystems ---"
grep sdcardfs /proc/filesystems || echo "НЕТ"
echo "--- ключевые символы ---"
for s in sdcardfs_permission sdcardfs_read_iter sdcardfs_write_iter sdcardfs_splice_read sdcardfs_mount sdcardfs_main_fops; do
    n=$(grep -c "$s" /proc/kallsyms 2>/dev/null)
    echo "$s = $n"
done

echo
echo "===== 2. СВОЙСТВА ====="
echo "external_storage.sdcardfs.enabled = [$(getprop external_storage.sdcardfs.enabled)]"
echo "ro.build.version.release = $(getprop ro.build.version.release)"
echo "ro.build.version.sdk = $(getprop ro.build.version.sdk)"
echo "ro.crypto.state = $(getprop ro.crypto.state)"
echo "persist.sys.vold.appsdcardfs = [$(getprop persist.sys.vold.appsdcardfs)]"

echo
echo "===== 3. МАУНТЫ: runtime / pass_through / mnt/user ====="
grep -E 'mnt/runtime|pass_through|/mnt/user' /proc/mounts

echo
echo "===== 4. МАУНТЫ: storage ====="
grep -E '/storage' /proc/mounts

echo
echo "===== 5. ЧТО В /mnt/runtime ====="
ls -la /mnt/runtime 2>&1
for d in default read write full; do
    echo "--- /mnt/runtime/$d/emulated ---"
    ls -la "/mnt/runtime/$d/emulated" 2>&1 | head -8
done

echo
echo "===== 6. mnt/user ====="
ls -la /mnt/user 2>&1
ls -la /mnt/user/0 2>&1
ls -la /mnt/pass_through 2>&1

echo
echo "===== 7. STATFS ====="
echo "--- /mnt/runtime/full/emulated ---"
stat -f /mnt/runtime/full/emulated 2>&1
echo "--- /mnt/user/0/emulated ---"
stat -f /mnt/user/0/emulated 2>&1

echo
echo "===== 8. СТРАНИЦЫ SPLICE ====="
for p in /mnt/runtime/full/emulated/0/Download /mnt/user/0/emulated/0/Download; do
    echo "--- $p ---"
    ls -la "$p" 2>&1 | head -5
done

echo
echo "===== 9. ZYGISK ====="
ls -la /data/adb/modules 2>&1
echo "--- zygisknext? ---"
ls -la /data/adb/modules/zygisksu 2>&1 | head -20
find /data/adb/modules -maxdepth 2 -name 'zygisk' -o -maxdepth 2 -name '*.so' 2>/dev/null | head -20
echo "--- ksud / ksu версия ---"
su -c 'ksud -V' 2>&1 | head -3

echo
echo "===== КОНЕЦ ====="
