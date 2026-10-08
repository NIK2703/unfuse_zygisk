#!/system/bin/sh
# vold-targets.sh — применимость ОБОИХ vold-хуков к КАЖДОМУ целевому образу vold.
#
# Зачем. Для libc применимость по образу проверяет tools/verify-hook-targets.py,
# а для vold не проверял никто: --selftest не вызывался ни одним прогоном, а
# применимость к файлу смотрели руками. Между тем обе программы умеют ответить
# на ФАЙЛЕ, ничего в него не записывая (ветка --file возвращается раньше записи):
#
#   vold-noacl  --file <vold> --selftest   .rela.plt <-> .plt взаимно однозначно
#                                          по ВСЕМ символам образа
#   vold-noacl  --file <vold> --dry-run    setxattr разрешается, трамплин цел
#   vold-fusefs --file <vold> --dry-run    mount и umount2 разрешаются, ОБА
#                                          трамплина целы, и структурно найден
#                                          ровно один вызов mount("/dev/fuse",
#                                          ..., "fuse", ..., MS_LAZYTIME)
#
# Третья строка — не то же самое, что первые две, и это и есть применимость
# mount-хука: без единственной найденной площадки вызова обработчику нечем
# отличить MountUserFuse() от AppFuseUtil::Mount(), и vold-fusefs отказывается
# ставить патч (код 2) — см. find_fuse_site() и MS_LAZYTIME в tools/vold-fusefs.c.
#
# --selftest при успехе МОЛЧИТ (логгирование из инструментов убрано, результат —
# код возврата), поэтому печатаются коды, а не вывод.
#
# Использование: sh vold-targets.sh [каталог-образов [vold-noacl [vold-fusefs]]]
#   каталог по умолчанию /data/local/tmp/voldimg
#   инструменты по умолчанию /data/local/tmp/vold-{noacl,fusefs}-test
# Код возврата: 0 все образы пригодны; 1 есть непригодный; 2 нечего проверять.

DIR="${1:-/data/local/tmp/voldimg}"
N="${2:-/data/local/tmp/vold-noacl-test}"
F="${3:-/data/local/tmp/vold-fusefs-test}"

[ -d "$DIR" ] || { echo "нет каталога $DIR" >&2; exit 2; }
[ -x "$N" ]   || { echo "нет инструмента $N" >&2; exit 2; }
[ -x "$F" ]   || { echo "нет инструмента $F" >&2; exit 2; }

# 0 на месте/ок, 1 трамплин цел, 2 не разобрать, 3 не записать, 4 нет файла
word() {
    case "$1" in
        0) echo ok ;;
        1) echo "трамплин-цел" ;;
        2) echo "НЕ-РАЗОБРАТЬ" ;;
        3) echo "НЕ-ЗАПИСАТЬ" ;;
        4) echo "НЕТ-ФАЙЛА" ;;
        *) echo "код-$1" ;;
    esac
}

printf '%-16s %-13s %-15s %-9s %s\n' "образ" "setxattr" "mount+umount2" "selftest" "вердикт"
echo "------------------------------------------------------------------"
echo

n=0
bad=0
for f in "$DIR"/*; do
    [ -f "$f" ] || continue
    name="${f##*/}"

    "$N" --file "$f" --dry-run  >/dev/null 2>&1; rs=$?
    "$F" --file "$f" --dry-run  >/dev/null 2>&1; rf=$?
    "$N" --file "$f" --selftest >/dev/null 2>&1; ss=$?

    n=$((n + 1))
    if [ "$rs" = 0 ] && [ "$rf" = 0 ] && [ "$ss" = 0 ]; then
        v="пригоден"
    else
        v="НЕПРИГОДЕН"
        bad=$((bad + 1))
    fi
    printf '%-16s %-13s %-15s %-9s %s\n' "$name" "$(word $rs)" "$(word $rf)" "$(word $ss)" "$v"
done

if [ "$n" = 0 ]; then
    echo "в $DIR нет файлов" >&2
    exit 2
fi

echo
echo "образов: $n, непригодных: $bad"
[ "$bad" = 0 ] || exit 1
