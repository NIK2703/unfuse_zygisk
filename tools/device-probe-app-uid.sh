#!/system/bin/sh
# probe-app.sh <pid> — проверка доступа ОТ ИМЕНИ uid приложения,
# внутри ЕГО mount namespace (то есть с sdcardfs, а не с FUSE).
#
# nsenter сам по себе не меняет личность, поэтому DAC-проверка через него
# ничего не доказывает: root обходит права. Здесь мы сначала заходим в
# namespace приложения, а затем сбрасываем uid/gid/группы через runas —
# ровно те, что Zygote выдал процессу (в группах обязательно есть 9997).

PID="$1"
[ -n "$PID" ] || { echo "usage: probe-app.sh <pid>"; exit 2; }
[ -r "/proc/$PID/status" ] || { echo "нет процесса $PID"; exit 2; }

U=$(awk '/^Uid:/{print $2}' "/proc/$PID/status")
G=$(awk '/^Gid:/{print $2}' "/proc/$PID/status")
GR=$(awk '/^Groups:/{for(i=2;i<=NF;i++) printf "%s%s", $i, (i<NF?",":"")}' "/proc/$PID/status")

echo "процесс $PID: uid=$U gid=$G groups=$GR"

exec nsenter -t "$PID" -m -- /data/local/tmp/runas "$U" "$G" "$GR" /system/bin/sh -c '
echo "--- стат корня и содержимого (от имени приложения) ---"
stat -c "%n mode=%a uid=%u gid=%g" /storage/emulated /storage/emulated/0 2>&1
echo "--- тип ФС ---"
stat -f -c "statfs=%T" /storage/emulated 2>&1
echo "--- листинг корня ---"
ls /storage/emulated 2>&1
echo "--- Android/data: сколько пакетов видит ---"
ls /storage/emulated/0/Android/data 2>/dev/null | wc -l
echo "--- Android/data: первые 15 ---"
ls /storage/emulated/0/Android/data 2>/dev/null | head -15
echo "--- Android/obb ---"
ls /storage/emulated/0/Android/obb 2>/dev/null | head -15
echo "--- запись в Download ---"
if touch /storage/emulated/0/Download/.probe_sdcardfs 2>/dev/null; then
    echo "WRITE_OK"
    rm -f /storage/emulated/0/Download/.probe_sdcardfs
else
    echo "WRITE_FAIL"
fi
echo "--- запись в чужой Android/data (ai.qwenlm.chat.android) ---"
if ls -d /storage/emulated/0/Android/data/ai.qwenlm.chat.android >/dev/null 2>&1; then
    echo "ЧУЖОЙ_КАТАЛОГ_ВИДЕН"
else
    echo "чужой каталог не виден"
fi
'
