#!/system/bin/sh
PKG="$1"
[ -n "$PKG" ] || { echo "укажите пакет"; exit 1; }

PID=$(pidof "$PKG" 2>/dev/null | awk '{print $1}')
[ -n "$PID" ] || { echo "не запущен: $PKG"; exit 1; }

echo "===== $PKG pid=$PID ====="
echo "--- uid/gid/groups процесса (нужен gid 9997 = AID_EVERYBODY) ---"
grep -E '^(Uid|Gid|Groups):' "/proc/$PID/status" 2>&1

echo
echo "--- есть ли setpriv ---"
command -v setpriv 2>&1

echo
echo "--- вход в namespace процесса (nsenter) ---"
nsenter -t "$PID" -m -- sh -c '
  echo "  statfs /storage/emulated/0:"
  stat -f -c "    type=%t" /storage/emulated/0 2>&1
  echo "  /storage/emulated (корень):"
  stat -f -c "    type=%t" /storage/emulated 2>&1
  echo "  ls /storage/emulated/0 | head -12:"
  ls /storage/emulated/0 2>&1 | head -12
  echo "  ls /storage/emulated/0/Android/data | head -8:"
  ls /storage/emulated/0/Android/data 2>&1 | head -8
' 2>&1

echo
echo "--- то же под uid приложения (DAC-проверка), если есть setpriv ---"
UID_APP=$(grep '^Uid:' "/proc/$PID/status" | awk '{print $2}')
if command -v setpriv >/dev/null 2>&1 && [ -n "$UID_APP" ]; then
    nsenter -t "$PID" -m -- setpriv --reuid="$UID_APP" --regid=9997 --groups=9997 -- sh -c '
      echo "    id: $(id)"
      echo "    запись в /storage/emulated/0:"
      touch /storage/emulated/0/.sdcardfs_probe 2>&1 && echo "      OK создан" && rm -f /storage/emulated/0/.sdcardfs_probe
      echo "    чтение чужого Android/data:"
      ls /storage/emulated/0/Android/data 2>&1 | head -5
    ' 2>&1
else
    echo "    (setpriv недоступен или uid не определён)"
fi

echo
echo "--- конец ---"
