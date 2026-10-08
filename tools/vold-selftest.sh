#!/system/bin/sh
# vold-selftest.sh — проверка ОБОИХ патчей vold: состояние на живом процессе и
# корректность разбора/эмиссии. Ни один прогон этого не делал: --check был
# только в module/status.sh, а --selftest не вызывался вообще, хотя именно он
# ловит расхождение .rela.plt с .plt и поломку в эмитируемых обработчиках.
#
# Использование: sh vold-selftest.sh [путь-vold-noacl [путь-vold-fusefs]]
#   по умолчанию — /data/local/tmp/{vold-noacl-test,vold-fusefs-test}
#   из final-verify.sh передаются установленные: $M/tools/vold-{noacl,fusefs}
#
# --selftest возвращает 0 МОЛЧА (логгирование из инструментов убрано, результат
# — код возврата, а не строки), поэтому печатаются коды, а не вывод инструментов.
#
# Коды у обоих инструментов: 0 патч на месте, 1 трамплин цел, 2 разбор,
# 3 запись, 4 нет vold.

N="${1:-/data/local/tmp/vold-noacl-test}"
F="${2:-/data/local/tmp/vold-fusefs-test}"
V="${3:-/system/bin/vold}"

echo "=== vold-noacl: разбор .rela.plt <-> .plt по ВСЕМ символам (--selftest) ==="
"$N" --file "$V" --selftest
echo "  rc=$? (ожидается 0)"

echo
echo "=== vold-noacl: setxattr пропатчен на живом vold (--check) ==="
"$N" --check
echo "  rc=$? (ожидается 0)"

echo
echo "=== vold-fusefs: эмиссия обработчиков mount/umount2 (--selftest) ==="
"$F" --selftest
echo "  rc=$? (ожидается 0)"

echo
echo "=== vold-fusefs: mount и umount2 пропатчены на живом vold (--check) ==="
"$F" --check
echo "  rc=$? (ожидается 0)"
