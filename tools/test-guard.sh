#!/system/bin/sh
#
# test-guard.sh — проверка сторожа ACL на устройстве (альтернативный путь).
#
# Что проверяется:
#   1. какой путь задействован и поднят ли сторож;
#   2. инвариант у /data/media/0 до вмешательства;
#   3. сторож возвращает запись 9997 после того, как её сотрут ровно так, как
#      это делает vold (tools/acl-break, стенд; в модуль не входит);
#   4. то же для каталога пакета — там vold пишет запись для uid пакета.
#
# Запуск:
#   adb push tools/test-guard.sh /data/local/tmp/
#   adb push <собранный acl-break>  /data/local/tmp/acl-break
#   adb shell su -c 'sh /data/local/tmp/test-guard.sh'
#
# Стенд собирается на хосте из tools/acl-break.c:
#   cc=.../aarch64-linux-android26-clang
#   $cc -std=c11 -Oz tools/acl-break.c -o /tmp/acl-break
#

M=/data/adb/modules/sdcardfs_restore
FIX="$M/tools/storage-fix"
BREAK=/data/local/tmp/acl-break
LOG=/data/adb/sdcardfs_restore.log
ROOT=/data/media/0

fails=0
ok()   { echo "  ok   $*"; }
bad()  { echo "  ОШИБКА $*"; fails=$((fails + 1)); }

echo "=== 1. сторож ==="
if pgrep -f "storage-fix --guard" >/dev/null 2>&1; then
    ok "процесс сторожа есть"
else
    bad "сторожа нет — проверка имеет смысл только на альтернативном пути"
    echo "       (включить: README §6.1; журнал: tail -20 $LOG)"
fi
grep сторож "$LOG" 2>/dev/null | tail -3 | sed 's/^/  лог  /'

echo
echo "=== 2. инвариант до вмешательства ==="
if "$FIX" --check "$ROOT"; then
    ok "у $ROOT запись 9997 на месте"
else
    bad "у $ROOT записи 9997 нет (сторож не справляется?)"
fi

echo
echo "=== 3. сторож против записи vold у /data/media/0 ==="
if [ ! -x "$BREAK" ]; then
    echo "  пропуск: нет $BREAK (см. шапку скрипта)"
else
    "$BREAK" "$ROOT" >/dev/null 2>&1
    if "$FIX" --check "$ROOT" >/dev/null 2>&1; then
        echo "  пропуск: ACL не удалось испортить"
    else
        ok "ACL испорчен — запись 9997 стёрта, как это делает vold"

        i=0
        while [ "$i" -lt 20 ]; do
            sleep 0.2
            if "$FIX" --check "$ROOT" >/dev/null 2>&1; then break; fi
            i=$((i + 1))
        done

        if "$FIX" --check "$ROOT"; then
            ok "сторож вернул запись 9997 за $i опрос(ов) по 0.2 с"
        else
            bad "сторож не вернул запись за 4 с"
        fi
    fi
fi

echo
echo "=== 4. сторож против записи vold у каталога пакета ==="
PKG=""
for d in "$ROOT"/Android/data/*/; do
    [ -d "$d" ] && PKG="${d%/}" && break
done

if [ -z "$PKG" ]; then
    echo "  пропуск: в $ROOT/Android/data нет ни одного каталога"
elif [ ! -x "$BREAK" ]; then
    echo "  пропуск: нет $BREAK"
else
    echo "  каталог: $PKG"
    "$BREAK" "$PKG" >/dev/null 2>&1
    if "$FIX" --check "$PKG" >/dev/null 2>&1; then
        echo "  пропуск: ACL не удалось испортить"
    else
        i=0
        while [ "$i" -lt 20 ]; do
            sleep 0.2
            if "$FIX" --check "$PKG" >/dev/null 2>&1; then break; fi
            i=$((i + 1))
        done
        if "$FIX" --check "$PKG"; then
            ok "сторож вернул запись 9997"
        else
            bad "сторож не вернул запись за 4 с"
        fi
    fi
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "ИТОГ: всё на месте"
    exit 0
fi
echo "ИТОГ: ошибок $fails"
exit 1
