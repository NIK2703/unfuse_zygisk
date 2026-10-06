#!/system/bin/sh
# probe-fuse-hide.sh <pid>
# Проверяет, СКРЫВАЕТ ли слой FUSE каталог Android/data для обычного uid,
# и что видит конкретный процесс (провайдер SAF, DocumentsUI и т.п.).
#
# Важно: nsenter сам личность не меняет. Чтобы получить осмысленный ответ,
# команда выполняется через runas от имени uid процесса — иначе root
# обходит и DAC, и проверки FUSE-демона.

PID="$1"
[ -n "$PID" ] || { echo "usage: probe-fuse-hide.sh <pid>"; exit 2; }
[ -r "/proc/$PID/status" ] || { echo "нет процесса $PID"; exit 2; }

NAME=$(tr -d '\0' < "/proc/$PID/cmdline" 2>/dev/null | cut -d' ' -f1)
U=$(awk '/^Uid:/{print $2}' "/proc/$PID/status")
G=$(awk '/^Gid:/{print $2}' "/proc/$PID/status")
GR=$(awk '/^Groups:/{for(i=2;i<=NF;i++) printf "%s%s", $i, (i<NF?",":"")}' "/proc/$PID/status")
FS=$(nsenter -t "$PID" -m -- stat -f -c %T /storage/emulated 2>&1)

echo "=============================================="
echo "pid=$PID  $NAME"
echo "uid=$U gid=$G groups=$GR"
echo "ФС /storage/emulated = $FS"
echo "--- от имени uid процесса (runas) ---"
nsenter -t "$PID" -m -- /data/local/tmp/runas "$U" "$G" "$GR" /system/bin/sh -c '
  echo -n "  ls /storage/emulated            : "
  ls /storage/emulated 2>&1 | tr "\n" " "; echo
  echo -n "  ls .../Android                  : "
  ls /storage/emulated/0/Android 2>&1 | tr "\n" " "; echo
  echo -n "  ls .../Android/data (кол-во)    : "
  ls /storage/emulated/0/Android/data 2>&1 | wc -l
  echo -n "  ls .../Android/obb (кол-во)     : "
  ls /storage/emulated/0/Android/obb 2>&1 | wc -l
  echo -n "  первые 6 в Android/data         : "
  ls /storage/emulated/0/Android/data 2>&1 | head -6 | tr "\n" " "; echo
'
echo "=============================================="
