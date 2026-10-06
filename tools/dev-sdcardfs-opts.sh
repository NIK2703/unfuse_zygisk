#!/system/bin/sh
# dev-sdcardfs-opts.sh — what the kernel reports for the module's sdcardfs mounts
# (requested mask/gid visible? superblocks distinct?). Sets path=sdcardfs then back to raw.

M=/data/adb/modules/unfuse_zygisk
CONF=/data/adb/modules/unfuse_zygisk/unfuse_zygisk.conf

echo "############ поднимаю основной путь ############"
printf 'path=sdcardfs\n' > "$CONF"
sh "$M/storage.sh" opts1 >/dev/null 2>&1
echo "rc=$?"

echo
echo "=== /proc/mounts: строки наших четырёх точек ==="
for p in /mnt/runtime/default/emulated /mnt/runtime/read/emulated \
         /mnt/runtime/write/emulated   /mnt/runtime/full/emulated; do
    echo "--- $p"
    grep " $p " /proc/mounts | sed 's/^/    /'
done

echo
echo "=== /proc/self/mountinfo: те же точки (major:minor = суперблок) ==="
for p in /mnt/runtime/default/emulated /mnt/runtime/read/emulated \
         /mnt/runtime/write/emulated   /mnt/runtime/full/emulated; do
    grep " $p " /proc/self/mountinfo | sed 's/^/    /'
done

echo
echo "=== поле 3 (major:minor) по точкам — разные ли суперблоки ==="
for p in /mnt/runtime/default/emulated /mnt/runtime/read/emulated \
         /mnt/runtime/write/emulated   /mnt/runtime/full/emulated; do
    m=$(grep " $p " /proc/self/mountinfo | head -1 | awk '{print $3}')
    printf '  %-32s %s\n' "$p" "${m:-—}"
done

echo
echo "=== то, что модуль просил у mount(2) ==="
echo "  default mask=6  gid=1015"
echo "  read    mask=23 gid=9997"
echo "  write   mask=7  gid=9997"
echo "  full    mask=7  gid=9997"

echo
echo "############ возвращаю path=raw ############"
printf 'path=raw\n' > "$CONF"
sh "$M/storage.sh" opts2 >/dev/null 2>&1
echo "rc=$?"
for p in /mnt/runtime/default/emulated /mnt/runtime/read/emulated \
         /mnt/runtime/write/emulated   /mnt/runtime/full/emulated; do
    t=""
    while read -r _dev mp ty _rest; do
        [ "$mp" = "$p" ] && t="$ty"
    done < /proc/mounts
    printf '  %-32s %s\n' "$p" "${t:-—}"
done
