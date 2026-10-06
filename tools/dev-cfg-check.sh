#!/system/bin/sh
# dev-cfg-check.sh — path-setting state after module install.

echo "=== /data/adb/modules/unfuse_zygisk/unfuse_zygisk.conf ==="
if [ -f /data/adb/modules/unfuse_zygisk/unfuse_zygisk.conf ]; then
    ls -la /data/adb/modules/unfuse_zygisk/unfuse_zygisk.conf
    echo "--- непустые, некомментарные строки ---"
    grep -v '^[[:space:]]*#' /data/adb/modules/unfuse_zygisk/unfuse_zygisk.conf | grep -v '^[[:space:]]*$'
else
    echo "НЕТ — customize.sh не скопировал конфиг"
fi

echo
echo "=== устаревшая метка ==="
if [ -e /data/adb/unfuse_zygisk.force_raw ]; then
    echo "force_raw НА МЕСТЕ"
else
    echo "force_raw нет"
fi

echo
echo "=== /data/adb/modules_update/unfuse_zygisk/ (ждёт перезагрузки) ==="
ls -la /data/adb/modules_update/unfuse_zygisk/ 2>&1

echo
echo "=== /data/adb/modules/unfuse_zygisk/ (работает сейчас) ==="
ls -la /data/adb/modules/unfuse_zygisk/ 2>&1

echo
echo "=== есть ли path-mode.sh в обоих ==="
for d in /data/adb/modules_update/unfuse_zygisk \
         /data/adb/modules/unfuse_zygisk; do
    if [ -x "$d/path-mode.sh" ]; then
        echo "  $d/path-mode.sh — есть, исполняемый"
    elif [ -f "$d/path-mode.sh" ]; then
        echo "  $d/path-mode.sh — есть, НЕ исполняемый"
    else
        echo "  $d/path-mode.sh — нет"
    fi
done
