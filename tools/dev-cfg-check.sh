#!/system/bin/sh
#
# dev-cfg-check.sh — состояние настройки пути после установки модуля.
#
# Запуск: su -c 'sh /data/local/tmp/dev-cfg-check.sh'

echo "=== /data/adb/sdcardfs_restore.conf ==="
if [ -f /data/adb/sdcardfs_restore.conf ]; then
    ls -la /data/adb/sdcardfs_restore.conf
    echo "--- непустые, некомментарные строки ---"
    grep -v '^[[:space:]]*#' /data/adb/sdcardfs_restore.conf | grep -v '^[[:space:]]*$'
else
    echo "НЕТ — customize.sh не скопировал конфиг"
fi

echo
echo "=== устаревшая метка ==="
if [ -e /data/adb/sdcardfs_restore.force_raw ]; then
    echo "force_raw НА МЕСТЕ"
else
    echo "force_raw нет"
fi

echo
echo "=== /data/adb/modules_update/sdcardfs_restore/ (ждёт перезагрузки) ==="
ls -la /data/adb/modules_update/sdcardfs_restore/ 2>&1

echo
echo "=== /data/adb/modules/sdcardfs_restore/ (работает сейчас) ==="
ls -la /data/adb/modules/sdcardfs_restore/ 2>&1

echo
echo "=== есть ли path-mode.sh в обоих ==="
for d in /data/adb/modules_update/sdcardfs_restore \
         /data/adb/modules/sdcardfs_restore; do
    if [ -x "$d/path-mode.sh" ]; then
        echo "  $d/path-mode.sh — есть, исполняемый"
    elif [ -f "$d/path-mode.sh" ]; then
        echo "  $d/path-mode.sh — есть, НЕ исполняемый"
    else
        echo "  $d/path-mode.sh — нет"
    fi
done
