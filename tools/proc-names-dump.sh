#!/system/bin/sh
# proc-names-dump.sh — чем vold отличается от соседей по имени, на этом устройстве.
#
# Зачем. Оба патчера находят vold по имени, и имя — единственный опознавательный
# признак: pid не годится (vold перезапускается), а vold.rc несёт
# reboot_on_failure, поэтому «пропатчили не тот процесс» — ошибка, которая не
# остаётся локальной. Правило имени поэтому надо брать с устройства, а не
# вспоминать, и этот скрипт печатает ровно то, из чего оно состоит:
#
#   - /proc/<pid>/exe: путь бинарника. Это основной путь (он же единственный,
#     который работает под root), и сравнивается у него имя целиком.
#   - /proc/<pid>/cmdline: argv с NUL-разделителями. Мир-читаемый, поэтому это
#     путь для не-root. vold запускается НЕ голым — с четырьмя
#     --*_context=… , так что имя приходится отрезать по первому пробелу.
#   - /proc/<pid>/comm: у vold там "binder:<pid>_<n>" — главный поток ушёл в
#     binder, и по comm vold не опознать вовсе (в vold-common.h это записано).
#
# Вторая и третья секции — проверка на СТОЛКНОВЕНИЕ по префиксу: печатаются все
# процессы, чьё имя начинается на "vold". Именно так нашёлся
# vold_prepare_subdirs — сосед, которого vold сам exec-ает: сравнение первых
# четырёх байт принимало и его. См. tools/proc-name.h и tools/proc-name-test.c.
#
# Запуск (нужен root, чтобы readlink по чужим /proc работал):
#   adb push tools/proc-names-dump.sh /data/local/tmp/ && \
#   adb shell "su 0 sh /data/local/tmp/proc-names-dump.sh"

VP=$(pidof vold)
echo "vold pid: $VP"
echo "--- cmdline (NUL shown as |) ---"
tr '\0' '|' < /proc/$VP/cmdline; echo
echo "--- comm ---"
cat /proc/$VP/comm
echo "--- exe ---"
readlink /proc/$VP/exe
echo "--- exe basename exact-compare test ---"
B=$(readlink /proc/$VP/exe); B=${B##*/}
if [ "$B" = vold ]; then echo "basename == vold : YES"; else echo "basename == vold : NO ($B)"; fi
echo
echo "--- every process whose cmdline basename STARTS WITH vold ---"
for d in /proc/[0-9]*; do
  p=${d#/proc/}
  # Процессы умирают прямо во время обхода, и тогда оболочка ругается на само
  # перенаправление (а не tr), так что проверка нужна до него.
  [ -r "$d/cmdline" ] || continue
  c=$(tr '\0' ' ' < "$d/cmdline")
  [ -z "$c" ] && continue
  a0=${c%% *}; b=${a0##*/}
  case "$b" in vold*) echo "pid=$p argv0=[$a0] basename=[$b]";; esac
done
echo
echo "--- same, for comm ---"
for d in /proc/[0-9]*; do
  p=${d#/proc/}
  [ -r "$d/comm" ] || continue
  c=$(cat "$d/comm")
  case "$c" in vold*) echo "pid=$p comm=[$c]";; esac
done
echo
echo "--- vold_prepare_subdirs binary present? ---"
ls -l /system/bin/vold_prepare_subdirs 2>/dev/null || echo "(нет такого файла)"
