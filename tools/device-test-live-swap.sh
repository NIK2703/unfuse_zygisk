#!/system/bin/sh
# Живой тест подмены: целевой пакет против нецелевого.

TARGET=com.mixplorer
CONTROL=com.aimp.player

echo "===== 0. синхронизируем конфиг в каталог модуля ====="
cp -f /data/adb/sdcardfs-apps.conf /data/adb/modules/sdcardfs_restore/sdcardfs-apps.conf
chmod 0644 /data/adb/modules/sdcardfs_restore/sdcardfs-apps.conf
ls -la /data/adb/modules/sdcardfs_restore/sdcardfs-apps.conf
echo "--- что теперь увидит модуль ---"
grep -vE "^[[:space:]]*(#|$)" /data/adb/modules/sdcardfs_restore/sdcardfs-apps.conf

echo
echo "===== 1. перезапускаем целевое приложение: $TARGET ====="
am force-stop "$TARGET" 2>&1
sleep 1
monkey -p "$TARGET" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
sleep 5

echo
echo "===== 2. то же для контрольного: $CONTROL ====="
am force-stop "$CONTROL" 2>&1
sleep 1
monkey -p "$CONTROL" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
sleep 5

show() {
    pkg="$1"; label="$2"
    echo
    echo "########## $label: $pkg ##########"
    PID=$(pidof "$pkg" 2>/dev/null | awk '{print $1}')
    if [ -z "$PID" ]; then
        echo "  процесс не найден"
        return
    fi
    echo "  pid=$PID"
    echo "  uid=$(stat -c %u /proc/$PID 2>/dev/null)"
    echo "  --- statfs /storage/emulated/0 (через namespace процесса) ---"
    stat -f "/proc/$PID/root/storage/emulated/0" 2>&1 | grep -E "Type|File:" | sed 's/^/    /'
    echo "  --- монтирования в namespace процесса ---"
    grep -E " /storage| /mnt/user/0|emulated" "/proc/$PID/mounts" 2>/dev/null | sed 's/^/    /'
}

show "$TARGET"  "ЦЕЛЕВОЙ (в конфиге)"
show "$CONTROL" "КОНТРОЛЬНЫЙ (не в конфиге)"

echo
echo "===== 3. что модуль написал в logcat ====="
logcat -d -s SdcardFsRestore 2>/dev/null | tail -40

echo
echo "===== 4. сравнение магии ====="
for pkg in "$TARGET" "$CONTROL"; do
    PID=$(pidof "$pkg" 2>/dev/null | awk '{print $1}')
    [ -z "$PID" ] && continue
    t=$(stat -f "/proc/$PID/root/storage/emulated/0" 2>/dev/null | grep -o "Type:.*")
    echo "  $pkg -> $t"
done
echo "  (sdcardfs = 0x5dca2df5, fuse = 0x65735546)"

echo
echo "########## конец ##########"
