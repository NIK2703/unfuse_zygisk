#!/system/bin/sh
#
# test-guard.sh — проверка сторожа ACL на устройстве (альтернативный путь).
#
# Что проверяется:
#   1. какой путь задействован и поднят ли сторож;
#   2. инвариант у /data/media/0 до вмешательства;
#   3. сторож возвращает запись 9997 после того, как её сотрут ровно так, как
#      это делает vold (tools/acl-break, стенд; в модуль не входит) — и у корня
#      тома, и у каталога пакета;
#   4. за пределы политики он не заходит.
#
# О реакции судим по журналу сторожа, а не по состоянию ACL сразу после поломки:
# сторож успевает вернуть запись быстрее, чем запустится следующая проверка, и
# «ACL уже цела» ничего не доказывает.
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
ok()  { echo "  ok   $*"; }
bad() { echo "  ОШИБКА $*"; fails=$((fails + 1)); }

fixes() { grep -c 'вернул запись 9997' "$LOG" 2>/dev/null; }

# Ломает ACL стендом и ждёт, пока сторож это заметит и починит.
break_and_wait() {
    before=$(fixes)
    if ! "$BREAK" "$1" >/dev/null 2>&1; then bad "стенд не смог испортить ACL у $1"; return 1; fi

    i=0
    while [ "$i" -lt 30 ]; do
        [ "$(fixes)" -gt "$before" ] && break
        sleep 0.2
        i=$((i + 1))
    done

    if [ "$(fixes)" -le "$before" ]; then
        bad "сторож не среагировал на правку у $1 за 6 с"
        return 1
    fi
    if ! "$FIX" --check "$1" >/dev/null 2>&1; then
        bad "сторож отреагировал, но --check у $1 всё ещё 1"
        return 1
    fi
    ok "$1 — вернул за ~$i опрос(ов) по 0.2 с"
    return 0
}

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

if [ ! -x "$BREAK" ]; then
    echo
    echo "=== 3-4. пропуск: нет $BREAK (см. шапку скрипта) ==="
else
    echo
    echo "=== 3. сторож против записи vold у корня тома ==="
    break_and_wait "$ROOT"

    echo
    echo "=== 4. сторож против записи vold у каталога пакета ==="
    PKG=""
    for d in "$ROOT"/Android/data/*/; do
        [ -d "$d" ] && PKG="${d%/}" && break
    done
    if [ -z "$PKG" ]; then
        echo "  пропуск: в $ROOT/Android/data нет ни одного каталога"
    else
        echo "  каталог: $PKG"
        break_and_wait "$PKG"
    fi

    echo
    echo "=== 5. граница политики: глубже Android/<x>/<пакет> сторож не смотрит ==="
    before=$(fixes)
    mkdir -p "$ROOT/DCIM/guard-test-dir" 2>/dev/null
    "$BREAK" "$ROOT/DCIM/guard-test-dir" >/dev/null 2>&1
    sleep 1.5
    if [ "$(fixes)" != "$before" ]; then
        bad "сторож среагировал на каталог вне политики"
    elif "$FIX" --check "$ROOT/DCIM/guard-test-dir" >/dev/null 2>&1; then
        bad "стенд не испортил ACL вне политики — проверка ничего не доказывает"
    else
        ok "каталог вне политики оставлен как есть (и стенд действительно стирает 9997)"
    fi
    rmdir "$ROOT/DCIM/guard-test-dir" 2>/dev/null
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "ИТОГ: всё на месте"
    exit 0
fi
echo "ИТОГ: ошибок $fails"
exit 1
