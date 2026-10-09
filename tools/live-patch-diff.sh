#!/system/bin/sh
# live-patch-diff.sh — прочитать r-xp-сегмент libc живого процесса и сравнить с
# самим файлом libc. Печатает вывод `cmp -l` (позиция old new, восьмерично).
#
# Отдельным файлом по той же причине, что и live-patch-pick.sh: вложенные
# кавычки через adb shell ломаются тихо.
#
# usage: live-patch-diff.sh <pid> <memskip> <count> <libc-path> <fileskip>
#   все смещения — в блоках по 4096 байт.
pid=$1; memskip=$2; cnt=$3; libc=$4; fileskip=$5

dd if=/proc/$pid/mem of=/data/local/tmp/lp-mem.bin bs=4096 skip=$memskip count=$cnt 2>/dev/null
dd if=$libc          of=/data/local/tmp/lp-file.bin bs=4096 skip=$fileskip count=$cnt 2>/dev/null

ls -la /data/local/tmp/lp-mem.bin /data/local/tmp/lp-file.bin >&2
cmp -l /data/local/tmp/lp-file.bin /data/local/tmp/lp-mem.bin

rm -f /data/local/tmp/lp-mem.bin /data/local/tmp/lp-file.bin
