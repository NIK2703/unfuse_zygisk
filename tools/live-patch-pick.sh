#!/system/bin/sh
# live-patch-pick.sh — выбрать процесс-ПРИЛОЖЕНИЕ нужного класса, у которого
# отображён модуль и есть r-xp-сегмент libc, и напечатать его границы.
#
# Зачем отдельным файлом: собирать это через adb shell вложенными кавычками —
# источник тихих поломок (пустой результат выглядит как «процесса нет»).
#
# Почему только приложение. Zygisk мапит модуль и в zygote, и в system_server,
# и в webview_zygote, но патч ставится в postAppSpecialize, то есть в процессе
# приложения. Системный кандидат дал бы ложное «не патчен».
#
# Вывод: одна строка  pid|first_start|first_off|last_end|name
#   first_start — начало первого r-xp-куска libc, first_off — его файловый
#   офсет, last_end — конец последнего r-xp-куска libc (куски идут подряд, та
#   же база файла, поэтому диапазон непрерывен).
# Код возврата 1 — подходящего процесса нет.
# usage: live-patch-pick.sh <eiclass>    (1 = ELF32, 2 = ELF64)

C="$1"
[ -n "$C" ] || exit 2

for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    [ -e "$p/exe" ] || continue
    c=$(od -An -tu1 -j4 -N1 "$p/exe" 2>/dev/null | tr -d ' ')
    [ "$c" = "$C" ] || continue
    grep -qi unfuse "$p/maps" 2>/dev/null || continue

    line=$(grep 'r-xp' "$p/maps" 2>/dev/null | grep -m1 'libc\.so')
    [ -n "$line" ] || continue

    # cmdline — NUL-разделённый и добит NUL до конца страницы, поэтому
    # tr '\0' ' ' даёт хвост из пробелов. Срезаем его: иначе имя «содержит
    # пробел», и фильтр ниже отверг бы любой процесс приложения.
    nm=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null | sed 's/ *$//')
    [ -n "$nm" ] || continue
    case "$nm" in *zygote*|system_server|*\ *) continue ;; esac
    case "$nm" in /*) continue ;; esac

    first_start=$(echo "$line" | cut -d- -f1)
    first_off=$(echo "$line" | awk '{print $3}')
    last_end=$(grep 'r-xp' "$p/maps" 2>/dev/null | grep 'libc\.so' | cut -d' ' -f1 | cut -d- -f2 | tail -1)

    echo "$pid|$first_start|$first_off|$last_end|$nm"
    exit 0
done

exit 1
