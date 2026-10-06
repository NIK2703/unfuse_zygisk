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
#   D. патч vold: стоит ли, и на чём сошёлся инвариант ACL.
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
echo "=== D. Патч vold ==="
# Коды vold-noacl: 0 — патч на месте, 1 — трамплин цел (патча нет),
# 2 — не разобрался, 3 — не записалось.
"$M/tools/vold-noacl" --check >/dev/null 2>&1
rc=$?
case "$rc" in
    0) echo "патч: на месте" ;;
    1) echo "патч: НЕТ — трамплин setxattr цел" ;;
    2) echo "патч: не удалось разобрать vold (код 2)" ;;
    3) echo "патч: не удалось записать (код 3)" ;;
    *) echo "патч: vold не найден (код $rc)" ;;
esac
echo "журнал патча:"
grep -E 'vold-noacl|патч vold' "$LOG" | tail -4 | sed 's/^/  /'
echo
echo "инвариант /data/media/0 (ожидается ОК):"
"$M/tools/storage-fix" --check /data/media/0 | sed 's/^/  /'
echo
echo "default-ACL /data/media/0 (ожидается GROUP ... id=9997):"
if [ -x /data/local/tmp/acl-dump ]; then
    /data/local/tmp/acl-dump /data/media/0 | sed 's/^/  /'
else
    echo "  нет /data/local/tmp/acl-dump — смотрите --check выше"
fi
