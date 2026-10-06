#!/system/bin/sh
#
# ab-probe.sh — снятие показаний A/B по гонке за default-ACL у /data/media/0.
#
# Запуск:  su -c 'sh /data/local/tmp/ab-probe.sh'
#
# Печатает ровно то, что решает спор:
#   1) default-ACL у /data/media/0        — 9997 (патч есть) или 1023 (vold переписал);
#   2) состояние патча в vold             — коды vold-noacl;
#   3) наследование новым файлом          — что реально получат приложения.

M=/data/adb/modules/sdcardfs_restore
LOG=/data/adb/sdcardfs_restore.log
DUMP=/data/local/tmp/acl-dump

echo "=== 1. default-ACL у /data/media/0 ==="
"$DUMP" /data/media/0 | grep -E 'access|default'

echo
echo "=== 2. патч vold ==="
if [ -x "$M/tools/vold-noacl" ]; then
    "$M/tools/vold-noacl" --check
    rc=$?
    case "$rc" in
        0) echo "ИТОГ: патч на месте" ;;
        1) echo "ИТОГ: патча НЕТ — трамплин setxattr цел" ;;
        2) echo "ИТОГ: не удалось разобрать vold (код 2)" ;;
        3) echo "ИТОГ: не удалось записать (код 3)" ;;
        *) echo "ИТОГ: vold не найден (код $rc)" ;;
    esac
else
    echo "ИТОГ: патчера нет — vold перепишет default-ACL"
fi

echo
echo "=== 3. наследование новым файлом ==="
rm -rf /data/media/0/_abprobe
mkdir -p /data/media/0/_abprobe
touch /data/media/0/_abprobe/f1
echo "-- каталог:"
"$DUMP" /data/media/0/_abprobe | grep default
echo "-- файл:"
"$DUMP" /data/media/0/_abprobe/f1 | grep 'access  GROUP'
rm -rf /data/media/0/_abprobe

echo
echo "=== 4. журнал патча ==="
grep -E 'vold-noacl|патч vold' "$LOG" | tail -6
