#!/system/bin/sh
#
# dev-cfg-reinstall.sh — проверка того, что настройка модуля переживает
# переустановку. Запускать ДО повторной установки архива.
#
#   1. записать в конфиг заведомо «нестандартное» значение (path=raw);
#   2. показать, что записалось;
#   3. после переустановки архива — dev-cfg-check.sh должен показать, что
#      customize.sh НЕ перезаписал файл (в нём остался path=raw, а не path=auto).
#
# Запуск: su -c 'sh /data/local/tmp/dev-cfg-reinstall.sh'

CONF=/data/adb/sdcardfs_restore.conf

echo "=== ДО: содержимое конфига ==="
cat "$CONF" 2>&1 | grep -v '^[[:space:]]*#' | grep -v '^[[:space:]]*$'

echo
echo "=== записываю path=raw ==="
{
    echo "# sdcardfs_restore.conf — изменён вручную для проверки переустановки"
    echo "path=raw"
} > "$CONF" && echo "записано"

echo
echo "=== ПОСЛЕ записи ==="
cat "$CONF"
