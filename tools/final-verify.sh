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
#   D. поздние проходы ACL по журналу загрузки и итоговый default-ACL
#      /data/media/0.
#
# Намеренно НЕ ждёт поздних проходов: они идут через 15, 45 и 120 секунд после
# sys.boot_completed, и ожидание в лоб превратило бы проверку в двухминутное
# молчание. Вместо этого проходы читаются из журнала — то есть проверяется то,
# что реально произошло при загрузке, а не синтетический прогон.
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
echo "=== D. Поздние проходы ACL по журналу ==="
echo "последние проходы:"
grep 'повтор ACL' "$LOG" | tail -3 | sed 's/^/  /'
N=$(grep -c 'повтор ACL' "$LOG")
echo "всего проходов в журнале: $N"
echo
echo "default-ACL /data/media/0 (ожидается GROUP(9997)):"
"$T" acl /data/media/0 | grep -E 'data/media/0|default' | sed 's/^/  /'
