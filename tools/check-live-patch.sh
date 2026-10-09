#!/usr/bin/env bash
# check-live-patch.sh — доказать, что модуль реально пропатчил libc В ЖИВОМ
# процессе приложения, сравнением его .text с нетронутым файлом libc.
#
# Зачем именно так. Модуль ставит патч в postAppSpecialize, то есть в ПРИЛОЖЕНИИ,
# а не в zygote: и zygote, и system_server остаются нетронутыми, и «zygote не
# патчен» ничего не доказывает. Нужен процесс приложения — и отдельно 32-битный,
# и 64-битный: ветки ABI разные (armeabi-v7a / arm64-v8a), и молчаливо
# нереализованная ветка выглядит как «всё на f2fs», потому что 32-битных
# приложений может просто не быть запущено.
#
# Метод. r-xp-сегмент libc из /proc/<pid>/maps сопоставляется с файловым
# смещением, читается сквозь /proc/<pid>/mem (dd) и сравнивается с самим файлом
# libc (cmp -l). Различающиеся байты и есть патч; они ложатся ровно на адреса
# целей. На патченном процессе libc разбит mprotect'ом на несколько r-xp-VMA —
# это подпись патча, но память непрерывна, поэтому чтение сквозное.
#
# Выбор процесса и само чтение вынесены на устройство (live-patch-pick.sh,
# live-patch-diff.sh): собирать их через adb shell вложенными кавычками — способ
# получить пустой результат и прочитать его как «процесса нет».
#
# Запуск (хост, adb в PATH):
#   bash tools/check-live-patch.sh 32
#   bash tools/check-live-patch.sh 64
# Переменные: ADB (по умолчанию adb), DEV (адрес:порт).
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

# adb — нативный Windows-бинарник и не понимает /e/... пути MSYS (тот же
# капкан, что у питона). Отдаём ему путь в Windows-виде.
winpath() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi
}

for s in live-patch-pick.sh live-patch-diff.sh; do
    "$ADB" push "$(winpath "$HERE/$s")" "/data/local/tmp/$s" >/dev/null 2>&1 || {
        echo "не удалось положить $s на устройство" >&2; exit 1; }
done

PICK=$("$ADB" shell "su 0 sh /data/local/tmp/live-patch-pick.sh $EICLASS" 2>/dev/null | tr -d '\r')
if [ -z "$PICK" ]; then
    echo "нет процесса класса $CLS с отображённым модулем и libc" >&2
    exit 1
fi

PID="${PICK%%|*}";    rest="${PICK#*|}"
START="${rest%%|*}";  rest="${rest#*|}"
OFF="${rest%%|*}";    rest="${rest#*|}"
END="${rest%%|*}";    NAME="${rest#*|}"

echo "класс=$CLS pid=$PID"
echo "  процесс: $NAME"
echo "  libc:    $LIBC"
echo "  r-xp:    $START..$END  файловый офсет $OFF"

# Арифметика — на хосте: адрес arm64 не влезает в 32-битную арифметику toybox sh
# (0x72beeb9000/4096 даёт отрицательное число).
read -r MEMSKIP CNT FILESKIP <<< "$(python - "$START" "$END" "$OFF" <<'PY'
import sys
start=int(sys.argv[1],16); end=int(sys.argv[2],16); off=int(sys.argv[3],16)
size=end-start
assert size % 4096 == 0 and start % 4096 == 0 and off % 4096 == 0
print(start//4096, size//4096, off//4096)
PY
)"

"$ADB" shell "su 0 sh /data/local/tmp/live-patch-diff.sh $PID $MEMSKIP $CNT $LIBC $FILESKIP" \
    > "$WORK/diff-$CLS.txt" 2>/dev/null

# Тот самый файл, с которым сравнивали: тянем с устройства, чтобы сопоставление
# имён шло с ним же, а не с локальной копией, которая может отстать.
"$ADB" pull "$LIBC" "$(winpath "$WORK/libc-$CLS.so")" >/dev/null 2>&1

cd "$WORK" || exit 1
python - "$(winpath "$ROOT")" "$CLS" "libc-$CLS.so" "diff-$CLS.txt" "$FILESKIP" <<'PY'
import json, os, struct, subprocess, sys, tempfile

root, cls, libc, difffile, fileskip = sys.argv[1:6]
base = int(fileskip) * 4096

# --- измеренные смещения (файловые, от начала файла libc) --------------------
rows = []
for line in open(difffile):
    p = line.split()
    if len(p) >= 3:
        try: i = int(p[0])
        except ValueError: continue
        rows.append(base + i - 1)
groups = []
GAP = 16          # внутри одного патча байт может случайно совпасть с оригиналом
                  # (тогда cmp даёт разрыв); цели же стоят куда дальше друг от друга
for off in rows:
    if groups and off - groups[-1][-1] - 1 <= GAP: groups[-1].append(off)
    else: groups.append([off])

# --- сегменты: vaddr <-> файловый офсет -------------------------------------
d = open(libc, 'rb').read()
segs = []
if d[4] == 2:                                   # ELFCLASS64
    e_phoff, = struct.unpack_from('<Q', d, 0x20)
    e_phentsize, e_phnum = struct.unpack_from('<HH', d, 0x36)
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        p_type, = struct.unpack_from('<I', d, o)
        p_offset, p_vaddr = struct.unpack_from('<QQ', d, o + 8)
        p_filesz, = struct.unpack_from('<Q', d, o + 32)
        if p_type == 1: segs.append((p_offset, p_vaddr, p_filesz))
else:                                           # ELFCLASS32
    e_phoff, = struct.unpack_from('<I', d, 0x1c)
    e_phentsize, e_phnum = struct.unpack_from('<HH', d, 0x2a)
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, _pa, p_filesz = struct.unpack_from('<IIIII', d, o)
        if p_type == 1: segs.append((p_offset, p_vaddr, p_filesz))

def vaddr_to_off(v):
    for po, pv, psz in segs:
        if pv <= v < pv + psz: return po + (v - pv)
    return None

# --- цели: тот же источник истины, что у всего проекта ----------------------
jpath = 'targets-%s.json' % cls
subprocess.run([sys.executable, os.path.join(root, 'tools', 'verify-hook-targets.py'),
                '--json', jpath, libc],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
targets = []
try:
    data = json.load(open(jpath))
    entries = next(iter(data.values()))
    for e in entries:
        if e.get('role') != 'root': continue
        off = vaddr_to_off(e['value'] & ~1)     # бит 0 — режим, патч по чётному
        if off is not None: targets.append((e['name'], off))
except Exception:
    pass

print(f"  изменённых байт: {len(rows)}   участков: {len(groups)}")
for g in groups:
    start, span = g[0], g[-1] - g[0] + 1
    hit = next((n for n, o in targets if o == start), None)
    label = hit if hit else ('?' if targets else '')
    print(f"    {start:#x}..{g[-1]:#x}  ({span} байт)  {label}")

if targets:
    want = {o for _, o in targets}
    got = {g[0] for g in groups}
    miss = [(n, o) for n, o in targets if o not in got]
    extra = sorted(got - want)
    print(f"  целей-корней в libc: {len(targets)}, пропатчено: {len(want & got)}")
    for n, o in miss:
        print(f"    НЕ пропатчена: {n} @ {o:#x}")
    for o in extra:
        print(f"    патч не на цели: {o:#x}")

sys.exit(0 if rows else 2)
PY
