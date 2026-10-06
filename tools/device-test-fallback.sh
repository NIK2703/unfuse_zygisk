#!/system/bin/sh
#
# device-test-fallback.sh — регрессия альтернативного пути модуля: приложения
# должны получать прямой доступ к внутренней памяти, даже когда sdcardfs
# недоступен.
#
# Запуск на устройстве под root:
#   adb push tools/device-test-fallback.sh /data/local/tmp/
#   adb push <aclprobe для своего ABI> /data/local/tmp/aclprobe
#   adb shell su -c 'sh /data/local/tmp/device-test-fallback.sh'
#
# Что делает:
#   1. накрывает /mnt/runtime/*/emulated tmpfs — под точками больше не sdcardfs,
#      как на ядре без CONFIG_SDCARD_FS;
#   2. подменяет mount(1) заглушкой, которая всегда падает, — ровно то, что
#      сделал бы mount -t sdcardfs на ядре без этой ФС;
#   3. прогоняет штатный storage.sh модуля: он должен не пройти gate
#      sdcardfs_ready() и уйти на шаг 3 — расставить ACL на сыром дереве;
#   4. перезапускает приложение, чтобы Zygote прошёл через модуль, и проверяет,
#      что модуль ушёл на сырой /data/media, а само приложение внутри своего
#      mount namespace может читать и писать /storage/emulated/0 под своим uid;
#   5. снимает tmpfs и перезапускает приложение — должен вернуться sdcardfs.
#
# tmpfs — стековый маунт: sdcardfs под ним цел, шаг 5 возвращает всё как было.
#
MODDIR=/data/adb/modules/sdcardfs_restore
LOG=/data/adb/sdcardfs_restore.log
PROBE=/data/local/tmp/aclprobe
FAKEBIN=/data/local/tmp/fakebin
APP=com.termux
ACT=$APP/.app.TermuxActivity
POINTS="default read write full"
SDCARDFS=sdcardfs

pid_of() { pidof "$1" | awk '{print $1}'; }

# Тип эффективного маунта: последняя запись /proc/mounts для пути. Только
# средствами шелла — stat(1) на стадиях модуля подменяется busybox-овским, у
# которого `-c %T` печатает UNKNOWN (см. storage.sh).
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

# Тип ФС, который видит сам процесс приложения (в его mount namespace).
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
logcat -d -s SdcardFsRestore:* | tail -5 | sed 's/^/  /'
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
logcat -d -s SdcardFsRestore:* | tail -5 | sed 's/^/  /'
printf '  /storage/emulated/0 в namespace приложения: %s\n' "$(fs_at "$PID" /storage/emulated/0)"
