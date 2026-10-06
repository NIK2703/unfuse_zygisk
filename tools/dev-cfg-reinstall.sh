#!/system/bin/sh
# dev-cfg-reinstall.sh — verify the config survives reinstall. Writes path=raw
# before reflashing; run BEFORE reinstalling, then dev-cfg-check.sh should show it kept.

CONF=/data/adb/modules/unfuse_zygisk/unfuse_zygisk.conf

echo "=== ДО: содержимое конфига ==="
cat "$CONF" 2>&1 | grep -v '^[[:space:]]*#' | grep -v '^[[:space:]]*$'

echo
echo "=== записываю path=raw ==="
{
    echo "# unfuse_zygisk.conf — изменён вручную для проверки переустановки"
    echo "path=raw"
} > "$CONF" && echo "записано"

echo
echo "=== ПОСЛЕ записи ==="
cat "$CONF"
