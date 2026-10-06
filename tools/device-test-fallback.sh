#!/system/bin/sh
# device-test-fallback.sh — fallback-path regression: apps must get direct internal
# storage even without sdcardfs. Run as root. Covers points with tmpfs, stubs mount(1)
# to fail, runs storage.sh (must fall back to raw ACLs), restarts the app via Zygote,
# then drops tmpfs (sdcardfs returns).
MODDIR=/data/adb/modules/unfuse_zygisk
LOG=/data/adb/unfuse_zygisk.log
PROBE=/data/local/tmp/aclprobe
FAKEBIN=/data/local/tmp/fakebin
APP=com.termux
ACT=$APP/.app.TermuxActivity
POINTS="default read write full"
SDCARDFS=sdcardfs

pid_of() { pidof "$1" | awk '{print $1}'; }

# Effective mount type: last /proc/mounts entry for the path. Pure shell, because
# during module stages stat(1) is busybox's, whose `-c %T` prints UNKNOWN.
fs_type() {
    t=""
    while read -r _dev mp ty _rest; do
        [ "$mp" = "$1" ] && t="$ty"
    done < /proc/mounts
    echo "$t"
}

restart_app() {
    am force-stop "$APP"
    sleep 1
    logcat -c
    am start -n "$ACT" >/dev/null 2>&1
    sleep 5
    pid_of "$APP"
}

# FS type the app process itself sees (in its mount namespace).
fs_at() { nsenter -t "$1" -m -- stat -f -c %T "$2" 2>&1; }

echo "############ АЛЬТЕРНАТИВНЫЙ ПУТЬ (sdcardfs недоступен) ############"
echo
echo "=== 1. точки под tmpfs ==="
for m in $POINTS; do
    mount -t tmpfs tmpfs "/mnt/runtime/$m/emulated" && echo "  tmpfs -> /mnt/runtime/$m/emulated"
done
for m in $POINTS; do
    printf '  %-38s %s\n' "/mnt/runtime/$m/emulated" "$(fs_type "/mnt/runtime/$m/emulated")"
done

echo
echo "=== 2. заглушка mount ==="
mkdir -p "$FAKEBIN"
printf '#!/system/bin/sh\nexit 1\n' > "$FAKEBIN/mount"
chmod 755 "$FAKEBIN/mount"
"$FAKEBIN/mount" -t sdcardfs /data/media /mnt/runtime/full/emulated
echo "  заглушка вернула exit=$?"

echo
echo "=== 3. штатный storage.sh модуля ==="
before=$(grep -c '' "$LOG" 2>/dev/null)
PATH="$FAKEBIN:$PATH" sh "$MODDIR/storage.sh" post-fs-data
echo "  exit=$?"
tail -n +"$((before + 1))" "$LOG" | sed 's/^/  /'

echo
echo "=== 4. приложение через Zygote ==="
PID=$(restart_app)
echo "  pid=$PID"
logcat -d -s UnfuseZygisk:* | tail -5 | sed 's/^/  /'
printf '  /storage/emulated/0 в namespace приложения: %s\n' "$(fs_at "$PID" /storage/emulated/0)"
echo "  --- изоляции Android/data нет (все каталоги реальные, не tmpfs) ---"
nsenter -t "$PID" -m -- grep -E ' /storage/emulated/0/Android/(data|obb) ' /proc/self/mounts \
    | awk '{printf "    %-6s %s\n", $3, $2}'
echo "  --- доступ под uid приложения ---"
nsenter -t "$PID" -m "$PROBE" verify /storage/emulated/0/Download 2>&1 | sed 's/^/  /'
nsenter -t "$PID" -m "$PROBE" list /storage/emulated/0/Android/data 2>&1 | sed 's/^/  /'
nsenter -t "$PID" -m "$PROBE" list /storage/emulated/0/Android/obb 2>&1 | sed 's/^/  /'

echo
echo "############ ВОЗВРАТ К ОСНОВНОМУ ПУТИ ############"
echo
echo "=== 5. снимаю tmpfs ==="
for i in 1 2 3; do
    for m in $POINTS; do
        p="/mnt/runtime/$m/emulated"
        [ "$(fs_type "$p")" = "$SDCARDFS" ] || umount "$p" 2>/dev/null
    done
done
for m in $POINTS; do
    printf '  %-38s %s\n' "/mnt/runtime/$m/emulated" "$(fs_type "/mnt/runtime/$m/emulated")"
done

echo
echo "=== 6. приложение снова на sdcardfs ==="
PID=$(restart_app)
echo "  pid=$PID"
logcat -d -s UnfuseZygisk:* | tail -5 | sed 's/^/  /'
printf '  /storage/emulated/0 в namespace приложения: %s\n' "$(fs_at "$PID" /storage/emulated/0)"
