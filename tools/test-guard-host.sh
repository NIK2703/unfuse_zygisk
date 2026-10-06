#!/usr/bin/env bash
#
# test-guard-host.sh — проверка логики сторожа (storage-fix --guard) на хосте.
#
# Устройство не нужно: GUARD_ROOT и GUARD_LOCK переопределяются при сборке
# (-DGUARD_ROOT=...), поэтому сторожа можно прогнать на каталоге в /tmp и
# убедиться в том, что на устройстве проверять дороже всего:
#
#   1. разовая правка и --check согласованы;
#   2. сторож поднимается и второй экземпляр не пускает (flock);
#   3. он возвращает запись 9997 после правки, сделанной ровно так, как это
#      делает vold (tools/acl-break.c) — и у корня тома, и у каталога пакета;
#   4. он подхватывает каталог, появившийся уже после его запуска;
#   5. за пределы политики он не заходит;
#   6. он НЕ зацикливается на собственных правках.
#
# Требуется: clang (или cc) и ФС с поддержкой POSIX ACL под /tmp.
#
#   sh tools/test-guard-host.sh        # или bash — скрипту нужен bash
#
set -u

SRC="$(cd -- "$(dirname -- "$0")/.." && pwd)"
CC="${CC:-clang}"
T="${TMPDIR:-/tmp}/guardtest"
R=$T/media
LOG=$T/guard.log
SF=$T/sf

fails=0
ok()  { echo "  ok    $*"; }
bad() { echo "  ОШИБКА $*"; fails=$((fails + 1)); }

rm -rf "$T"
mkdir -p "$R/0/Android/data/com.foo/files" "$R/0/Android/obb" "$R/0/Android/media" "$R/0/DCIM"

"$CC" -std=c11 -Wall -Wextra -Wno-unused-parameter -O1 \
      -DGUARD_ROOT="\"$R\"" -DGUARD_LOCK="\"$T/guard.lock\"" \
      -o "$SF" "$SRC/tools/storage-fix.c" || exit 1
"$CC" -std=c11 -O1 -o "$T/acl-break" "$SRC/tools/acl-break.c" || exit 1
echo "собрано: $SF (корень сторожа = $R)"

# Ждём, пока --check начнёт отвечать 0, не дольше limit*0.1 с.
wait_ok() {
    local d=$1 limit=$2 i=0
    while [ "$i" -lt "$limit" ]; do
        if "$SF" --check "$d" >/dev/null 2>&1; then echo "$i"; return 0; fi
        sleep 0.1
        i=$((i + 1))
    done
    echo "$i"
    return 1
}

# Ждём, пока число правок в журнале превысит n. Сторож может успеть раньше, чем
# проверка --check, поэтому о реакции судим по журналу, а не по состоянию ACL.
wait_fix() {
    local n=$1 limit=$2 i=0
    while [ "$i" -lt "$limit" ]; do
        [ "$(grep -c 'вернул запись 9997' "$LOG")" -gt "$n" ] && { echo "$i"; return 0; }
        sleep 0.1
        i=$((i + 1))
    done
    echo "$i"
    return 1
}

# Ломает ACL стендом и убеждается, что сторож это починил.
break_and_wait() {
    local d=$1 before n
    before=$(grep -c 'вернул запись 9997' "$LOG")
    if ! "$T/acl-break" "$d" >/dev/null; then bad "стенд не смог испортить ACL у $d"; return 1; fi
    if ! n=$(wait_fix "$before" 30); then bad "сторож не среагировал на правку у $d за 3 с"; return 1; fi
    if ! "$SF" --check "$d" >/dev/null 2>&1; then bad "после правки сторожа --check всё ещё 1 у $d"; return 1; fi
    ok "$d — вернул за $((n / 10)).$((n % 10)) с"
    return 0
}

echo
echo "=== 1. разовая правка и проверка ==="
if "$SF" --check "$R/0" >/dev/null 2>&1; then bad "до правки --check уже отвечает 0"; else ok "до правки --check отвечает 1"; fi
"$SF" --traverse "$R" >/dev/null
"$SF" "$R/0" >/dev/null
if "$SF" --check "$R/0" >/dev/null 2>&1; then ok "после правки --check отвечает 0"; else bad "после правки --check отвечает 1"; fi

echo
echo "=== 2. запуск сторожа ==="
"$SF" --guard >"$LOG" 2>&1 &
GUARD=$!
sleep 0.5
if grep -q "под сторожем каталогов" "$LOG"; then
    ok "сторож поднялся: $(grep 'под сторожем' "$LOG")"
else
    bad "сторож не отрапортовал о запуске"; cat "$LOG"
fi

echo
echo "=== 3. второй экземпляр не поднимается ==="
out="$("$SF" --guard 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && echo "$out" | grep -q "уже работает"; then
    ok "второй экземпляр вышел: $out"
else
    bad "второй экземпляр повёл себя иначе (rc=$rc): $out"
fi

echo
echo "=== 4. сторож против записи vold у корня тома ==="
break_and_wait "$R/0"

echo
echo "=== 5. сторож против записи vold у каталога пакета ==="
break_and_wait "$R/0/Android/data/com.foo"

echo
echo "=== 6. каталог, появившийся после запуска сторожа ==="
mkdir -p "$R/0/Android/data/com.bar/files"
sleep 0.3
if "$SF" --check "$R/0/Android/data/com.bar" >/dev/null 2>&1; then
    ok "новый каталог пакета сразу получил ACL"
else
    bad "новый каталог пакета остался без ACL"
fi
break_and_wait "$R/0/Android/data/com.bar"

echo
echo "=== 7. граница политики: глубже Android/<x>/<пакет> сторож не смотрит ==="
mkdir -p "$R/0/DCIM/newdir"
before=$(grep -c 'вернул запись 9997' "$LOG")
"$T/acl-break" "$R/0/DCIM/newdir" >/dev/null
sleep 1.5
if "$SF" --check "$R/0/DCIM/newdir" >/dev/null 2>&1; then
    bad "сторож тронул каталог вне политики — значит смотрит слишком глубоко"
elif [ "$(grep -c 'вернул запись 9997' "$LOG")" != "$before" ]; then
    bad "сторож среагировал на каталог вне политики"
else
    ok "каталог вне политики оставлен как есть (и стенд действительно стирает 9997)"
fi

echo
echo "=== 8. сходимость: нет цикла событий на своих же правках ==="
fixes=$(grep -c "вернул запись 9997" "$LOG")
if [ "$fixes" -le 6 ]; then
    ok "правок за сеанс: $fixes — цикла нет"
else
    bad "правок за сеанс: $fixes — похоже на цикл событий"
fi

kill "$GUARD" 2>/dev/null
wait "$GUARD" 2>/dev/null

echo
echo "=== журнал сторожа ==="
sed 's/^/  /' "$LOG"

echo
if [ "$fails" -eq 0 ]; then echo "ИТОГ: всё сошлось"; exit 0; fi
echo "ИТОГ: ошибок $fails"
exit 1
