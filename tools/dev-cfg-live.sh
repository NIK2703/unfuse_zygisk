#!/system/bin/sh
# dev-cfg-live.sh — live check of step 0 with the new build: runs storage.sh across
# three config/legacy-marker combos; ends on path=raw for fallback inspection.

M=/data/adb/modules_update/unfuse_zygisk
CONF=/data/adb/modules/unfuse_zygisk/unfuse_zygisk.conf
LEGACY=/data/adb/unfuse_zygisk.force_raw
LOG=/data/adb/unfuse_zygisk.log

mark() { wc -l < "$LOG" 2>/dev/null || echo 0; }

show_run() {
    from=$1
    echo "  --- новые строки журнала ---"
    tail -n +"$((from + 1))" "$LOG" | sed 's/^/    /'
}

echo "############################################################"
echo "# 1. конфиг path=raw + метка force_raw: конфиг должен быть"
echo "#    назван главным, режим — raw"
echo "############################################################"
printf 'path=raw\n' > "$CONF"
[ -e "$LEGACY" ] || touch "$LEGACY"
n=$(mark)
sh "$M/storage.sh" live1 >/dev/null 2>&1
echo "  rc=$?"
show_run "$n"

echo
echo "############################################################"
echo "# 2. конфиг path=sdcardfs + метка force_raw: конфиг главнее,"
echo "#    режим sdcardfs — метка НЕ должна перебить конфиг"
echo "############################################################"
printf 'path=sdcardfs\n' > "$CONF"
n=$(mark)
sh "$M/storage.sh" live2 >/dev/null 2>&1
echo "  rc=$?"
show_run "$n"

echo
echo "############################################################"
echo "# 3. конфиг path=raw, метки НЕТ: режим raw только из конфига"
echo "############################################################"
printf 'path=raw\n' > "$CONF"
rm -f "$LEGACY"
n=$(mark)
sh "$M/storage.sh" live3 >/dev/null 2>&1
echo "  rc=$?"
show_run "$n"

echo
echo "=== итог: что стоит на точках ==="
for p in /mnt/runtime/default/emulated /mnt/runtime/read/emulated \
         /mnt/runtime/write/emulated   /mnt/runtime/full/emulated; do
    t=""
    while read -r _dev mp ty _rest; do
        [ "$mp" = "$p" ] && t="$ty"
    done < /proc/mounts
    printf '  %-32s %s\n' "$p" "${t:-—}"
done
echo "  конфиг: $(grep -v '^[[:space:]]*#' "$CONF" | grep -v '^[[:space:]]*$' | tr '\n' ' ')"
echo "  метка:  $([ -e "$LEGACY" ] && echo 'на месте' || echo 'нет')"
