#!/system/bin/sh
# Does a given app really see internal storage through sdcardfs?
# Usage: sh verify-app.sh <package>

PKG="$1"
[ -n "$PKG" ] || { echo "укажите пакет"; exit 1; }

PID=$(pidof "$PKG" 2>/dev/null | awk '{print $1}')
if [ -z "$PID" ]; then
    echo "ПРОЦЕСС НЕ ЗАПУЩЕН: $PKG"
    exit 1
fi

echo "===== $PKG pid=$PID ====="

echo "--- монтирования в namespace процесса ---"
grep -E ' /storage| /mnt/user/0/emulated| /mnt/runtime' "/proc/$PID/mounts" 2>&1 | head -14

echo
echo "--- тип ФС под /storage/emulated/0 (через namespace процесса) ---"
stat -f -c '  type=%t (%T)' "/proc/$PID/root/storage/emulated/0" 2>&1

echo "--- тип ФС под /storage/emulated (корень) ---"
stat -f -c '  type=%t (%T)' "/proc/$PID/root/storage/emulated" 2>&1

echo
echo "--- права /storage/emulated/0 ---"
stat -c '  %n mode=%a uid=%u gid=%g ctx=%C' "/proc/$PID/root/storage/emulated/0" 2>&1

echo
echo "--- видно ли Android/data (первые 8 записей) ---"
ls -la "/proc/$PID/root/storage/emulated/0/Android/data" 2>&1 | head -10

echo
echo "--- видно ли Android/obb ---"
ls -la "/proc/$PID/root/storage/emulated/0/Android/obb" 2>&1 | head -6

echo
echo "--- корень памяти ---"
ls -la "/proc/$PID/root/storage/emulated/0" 2>&1 | head -12

echo
echo "--- конец ---"
