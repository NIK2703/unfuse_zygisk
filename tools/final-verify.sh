#!/system/bin/sh
#
# final-verify.sh — итоговая проверка модуля на устройстве.
#
# Запуск:  su -c 'sh /data/local/tmp/final-verify.sh'
#
# Проверяет всё, что можно проверить без ожидания:
#
#   A. самопроверка хука libc — 28 проверок;
#   B. сквозная проверка из namespace живого приложения (плюс самопроверка);
#   C. обнуление «остальных» в ACL у storage-fix;
#   D. сторож ACL: жив ли процесс, что он вернул и на чём сошёлся инвариант.
#
# Требует, чтобы на устройстве уже лежали hookselftest и device-e2e.sh:
#   adb push out/hookselftest-arm64 /data/local/tmp/hookselftest
#   adb push tools/device-e2e.sh    /data/local/tmp/

T=/data/local/tmp/hookselftest
M=/data/adb/modules/sdcardfs_restore
LOG=/data/adb/sdcardfs_restore.log

if [ ! -x "$T" ]; then
    echo "нет $T — сначала: adb push out/hookselftest-arm64 $T && adb shell chmod 755 $T"
    exit 1
fi

echo "=== A. Самопроверка хука libc ==="
"$T" | tail -3

echo
echo "=== B. Сквозная проверка из namespace приложения ==="
sh /data/local/tmp/device-e2e.sh 2>&1 | tail -8

echo
echo "=== C. Обнуление «остальных» в ACL (storage-fix) ==="
sh /data/local/tmp/test-storage-fix.sh 2>&1 | grep -E '^/data.*OTHER|открывает' | head -6

echo
echo "=== D. Сторож ACL ==="
if pgrep -f 'storage-fix --guard' >/dev/null 2>&1; then
    echo "процесс сторожа: есть"
else
    echo "процесс сторожа: НЕТ (на основном пути его и не должно быть — см. README §3.3)"
fi
echo "что он вернул:"
grep сторож "$LOG" | tail -4 | sed 's/^/  /'
echo
echo "инвариант /data/media/0 (ожидается ОК):"
"$M/tools/storage-fix" --check /data/media/0 | sed 's/^/  /'
echo
echo "default-ACL /data/media/0:"
"$T" acl /data/media/0 | grep -E 'data/media/0|default' | sed 's/^/  /'
