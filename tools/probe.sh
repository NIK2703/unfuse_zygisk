#!/system/bin/sh
# Диагностический зонд: состояние sdcardfs / FUSE на устройстве.
# Запуск:  su -c 'sh /data/local/tmp/probe.sh'

echo "===== probe: $(date) ====="
echo "--- id ---"
id

echo
echo "--- свойства ---"
echo "release          = $(getprop ro.build.version.release)"
echo "sdk              = $(getprop ro.build.version.sdk)"
echo "fingerprint      = $(getprop ro.build.fingerprint)"
echo "sdcardfs.enabled = [$(getprop external_storage.sdcardfs.enabled)]"
echo "vold.app_data_isolation = [$(getprop persist.sys.vold_app_data_isolation_enabled)]"

echo
echo "--- /proc/filesystems ---"
grep -i sdcardfs /proc/filesystems || echo "sdcardfs НЕ зарегистрирован"

echo
echo "--- точки монтирования (/proc/mounts) ---"
grep -E 'mnt/runtime|mnt/pass_through|mnt/user|/storage|mnt/androidwritable|mnt/installer' /proc/mounts

echo
echo "--- сколько маунтов sdcardfs ---"
grep -c sdcardfs /proc/mounts

echo
echo "--- /data/media ---"
ls -ld /data/media /data/media/0 2>&1

echo
echo "--- содержимое /mnt/runtime/full/emulated (верхний уровень) ---"
ls -la /mnt/runtime/full/emulated 2>&1 | head -10

echo
echo "--- содержимое /mnt/user/0/emulated ---"
ls -la /mnt/user/0/emulated 2>&1 | head -10

echo
echo "--- /storage ---"
ls -la /storage 2>&1 | head -10

echo
echo "--- statfs /mnt/runtime/full/emulated/0 ---"
stat -f /mnt/runtime/full/emulated/0 2>&1

echo
echo "--- statfs /mnt/user/0/emulated/0 ---"
stat -f /mnt/user/0/emulated/0 2>&1

echo
echo "--- sdcardfs_splice_read в kallsyms ---"
if [ -r /proc/kallsyms ]; then
    grep -c sdcardfs_splice_read /proc/kallsyms
else
    echo "kallsyms недоступен"
fi

echo
echo "===== конец ===== "
