#!/system/bin/sh
# dev-auto-live.sh — live check of auto path selection, then strict sdcardfs.

M=/data/adb/modules_update/unfuse_zygisk
CONF=/data/adb/modules/unfuse_zygisk/unfuse_zygisk.conf
LOG=/data/adb/unfuse_zygisk.log

mark() { wc -l < "$LOG" 2>/dev/null || echo 0; }

show_run() {
    echo "  --- новые строки журнала ---"
    tail -n +"$(( $1 + 1 ))" "$LOG" | sed 's/^/    /'
}

points() {
    echo "  --- что стоит на точках ---"
    for p in /mnt/runtime/default/emulated /mnt/runtime/read/emulated \
             /mnt/runtime/write/emulated   /mnt/runtime/full/emulated; do
        t=""
        while read -r _dev mp ty _rest; do
            [ "$mp" = "$p" ] && t="$ty"
        done < /proc/mounts
        printf '    %-32s %s\n' "$p" "${t:-—}"
    done
}

echo "############################################################"
echo "# 1. path=auto — приоритет у sdcardfs, проверка по факту"
echo "############################################################"
printf 'path=auto\n' > "$CONF"
n=$(mark)
sh "$M/storage.sh" auto1 >/dev/null 2>&1
echo "  rc=$?"
show_run "$n"
points

echo
echo "############################################################"
echo "# 2. path=sdcardfs — строгий режим, проверка должна пройти"
echo "############################################################"
printf 'path=sdcardfs\n' > "$CONF"
n=$(mark)
sh "$M/storage.sh" strict1 >/dev/null 2>&1
echo "  rc=$?"
show_run "$n"

echo
echo "=== опции маунтов: что ядро отдало против того, что просили ==="
for p in /mnt/runtime/default/emulated /mnt/runtime/read/emulated \
         /mnt/runtime/write/emulated   /mnt/runtime/full/emulated; do
    echo "--- $p"
    grep " $p " /proc/mounts | awk '{print "    " $4}'
done

echo
echo "############################################################"
echo "# 3. возвращаю path=acl — то состояние, что было до проверки"
echo "############################################################"
printf 'path=acl\n' > "$CONF"
n=$(mark)
sh "$M/storage.sh" back1 >/dev/null 2>&1
echo "  rc=$?"
show_run "$n"
points
