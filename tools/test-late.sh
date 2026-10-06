#!/system/bin/sh
#
# test-late.sh — проверка позднего повтора из service.sh.
#
# Гонка: vold после storage.sh ставит на /data/media/0 default-ACL с записью для
# 1023. Поздний повтор (после boot_completed) должен вернуть туда 9997.

T=/data/local/tmp/hookselftest
M=/data/adb/modules/sdcardfs_restore

echo "=== ДО: default-ACL /data/media/0 ==="
"$T" acl /data/media/0

echo
echo "=== запуск service.sh (он должен вернуться сразу) ==="
START=$(date +%s)
sh "$M/service.sh"
echo "service.sh вернулся за $(( $(date +%s) - START )) с — фон не блокирует init"

echo
echo "=== ждём поздний повтор (boot_completed уже 1, затем пауза 15 с) ==="
sleep 22

echo
echo "=== ПОСЛЕ: default-ACL /data/media/0 (ожидается GROUP(9997)) ==="
"$T" acl /data/media/0

echo
echo "=== хвост журнала ==="
tail -8 /data/adb/sdcardfs_restore.log
