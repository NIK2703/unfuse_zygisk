#!/usr/bin/env bash
# check-live-patch.sh — доказать, что модуль реально пропатчил libc В ЖИВОМ
# процессе приложения, сравнением его .text с нетронутым файлом libc.
#
# Зачем именно так. Модуль ставит патч в postAppSpecialize, то есть в ПРИЛОЖЕНИИ,
# а не в zygote: оба zygote остаются нетронутыми, и «zygote не патчен» ничего не
# доказывает. Нужен процесс приложения — и отдельно 32-битный, и 64-битный:
# ветки ABI разные (armeabi-v7a / arm64-v8a), и молчаливо нереализованная ветка
# выглядит как «всё на f2fs», потому что 32-битных приложений может просто не
# быть запущено.
#
# Метод. r-xp-сегмент libc из /proc/<pid>/maps сопоставляется с файловым
# смещением, читается сквозь /proc/<pid>/mem (dd) и сравнивается с самим файлом
# libc (cmp -l). Различающиеся байты и есть патч; они ложатся ровно на адреса
# целей. На патченном процессе libc разбит mprotect'ом на несколько r-xp-VMA —
# это подпись патча, но память непрерывна, поэтому чтение сквозное.
#
# Запуск (хост, нужен adb в PATH):
#   bash tools/check-live-patch.sh 32
#   bash tools/check-live-patch.sh 64
# Код возврата: 0 — патч найден; 1 — процесса нет; 2 — процесс есть, патча нет.
set -uo pipefail

CLS="${1:-}"
case "$CLS" in
    32|64) ;;
    *) echo "usage: $0 {32|64}" >&2; exit 64 ;;
esac

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/.." && pwd)"
WORK="$ROOT/out/live-patch"
mkdir -p "$WORK"

ADB="${ADB:-adb}"
DEV="${DEV:-10.243.159.191:5555}"
"$ADB" connect "$DEV" >/dev/null 2>&1

if [ "$CLS" = 64 ]; then
    LIBC=/apex/com.android.runtime/lib64/bionic/libc.so
    EICLASS=2          # e_ident[EI_CLASS]: 2 = ELFCLASS64, 1 = ELFCLASS32
else
    LIBC=/apex/com.android.runtime/lib/bionic/libc.so
    EICLASS=1
fi

# Кандидат: процесс нужного класса, с отображённым модулем и с r-xp libc.
PICK=$("$ADB" shell "su 0 sh -c '
for p in /proc/[0-9]*; do
  pid=\${p#/proc/}
  [ -e \"\$p/exe\" ] || continue
  c=\$(od -An -tu1 -j4 -N1 \"\$p/exe\" 2>/dev/null | tr -d \" \")
  [ \"\$c\" = $EICLASS ] || continue
  grep -qi unfuse \"\$p/maps\" 2>/dev/null || continue
  line=\$(grep \"r-xp\" \"\$p/maps\" 2>/dev/null | grep -m1 libc.so)
  [ -n \"\$line\" ] || continue
  nm=\$(tr \"\\0\" \" \" < \"\$p/cmdline\" 2>/dev/null)
  case \"\$nm\" in *zygote*) continue;; esac
  echo \"\$pid|\$line|\$nm\"
  break
done'" 2>/dev/null | tr -d '\r')

if [ -z "$PICK" ]; then
    echo "нет процесса класса $CLS с отображённым модулем и libc" >&2
    exit 1
fi

PID="${PICK%%|*}"
rest="${PICK#*|}"
MAPLINE="${rest%%|*}"
NAME="${rest#*|}"

FIRST_START=$(echo "$MAPLINE" | cut -d- -f1)
FIRST_OFF=$(echo "$MAPLINE" | awk '{print $3}')

# Конец непрерывного диапазона: максимальный адрес среди r-xp-кусков libc.
LAST_END=$("$ADB" shell "su 0 sh -c 'grep \"r-xp\" /proc/$PID/maps | grep libc.so | cut -d\" \" -f1 | cut -d- -f2 | tail -1'" 2>/dev/null | tr -d '\r')

echo "класс=$CLS pid=$PID"
echo "  процесс: $NAME"
echo "  libc:    $LIBC"
echo "  r-xp:    $FIRST_START..$LAST_END  файловый офсет $FIRST_OFF"

# Арифметика — на хосте: адрес arm64 не влезает в 32-битную арифметику toybox sh.
read -r MEMSKIP CNT FILESKIP <<< "$(python - "$FIRST_START" "$LAST_END" "$FIRST_OFF" <<'PY'
import sys
start=int(sys.argv[1],16); end=int(sys.argv[2],16); off=int(sys.argv[3],16)
size=end-start
assert size % 4096 == 0 and start % 4096 == 0 and off % 4096 == 0
print(start//4096, size//4096, off//4096)
PY
)"

"$ADB" shell "su 0 sh -c '
pid=$PID
dd if=/proc/\$pid/mem of=/data/local/tmp/lp-mem.bin bs=4096 skip=$MEMSKIP count=$CNT 2>/dev/null
dd if=$LIBC of=/data/local/tmp/lp-file.bin bs=4096 skip=$FILESKIP count=$CNT 2>/dev/null
cmp -l /data/local/tmp/lp-file.bin /data/local/tmp/lp-mem.bin
rm -f /data/local/tmp/lp-mem.bin /data/local/tmp/lp-file.bin'" > "$WORK/diff-$CLS.txt" 2>/dev/null

cd "$WORK" || exit 1
python - "$FILESKIP" "diff-$CLS.txt" <<'PY'
import sys
base=int(sys.argv[1])*4096
rows=[]
for line in open(sys.argv[2]):
    p=line.split()
    if len(p)>=3:
        try: i=int(p[0])
        except ValueError: continue
        rows.append(base+i-1)
groups=[]
for off in rows:
    if groups and off==groups[-1][-1]+1: groups[-1].append(off)
    else: groups.append([off])
print(f"  изменённых байт: {len(rows)}   участков: {len(groups)}")
for g in groups:
    print(f"    {g[0]:#x}..{g[-1]:#x}  ({len(g)} байт)")
sys.exit(0 if rows else 2)
PY
