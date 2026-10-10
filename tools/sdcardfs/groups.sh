#!/system/bin/sh
# groups.sh — which groups real app processes carry. The module's sdcardfs
# mounts hand out their gid to everyone, so it is 9997 (AID_EVERYBODY) that
# matters; 1023 (AID_MEDIA_RW) is shown for comparison with a stock FUSE mount.

echo "--- группы процессов приложений (ищем 1023 и 9997) ---"
printf '%-8s %-8s %-6s %-6s %s\n' PID UID 1023 9997 "группы"

n1023=0
n9997=0
total=0
for p in $(ps -A -o PID,UID | awk '$2 >= 10000 && $2 < 20000 {print $1}'); do
    [ -r "/proc/$p/status" ] || continue
    uid=$(awk '/^Uid:/{print $2}' "/proc/$p/status" 2>/dev/null)
    g=$(awk '/^Groups:/{for (i = 2; i <= NF; i++) printf "%s ", $i}' "/proc/$p/status" 2>/dev/null)
    has1023=no
    has9997=no
    for x in $g; do
        [ "$x" = "1023" ] && has1023=yes
        [ "$x" = "9997" ] && has9997=yes
    done
    [ "$has1023" = "yes" ] && n1023=$((n1023 + 1))
    [ "$has9997" = "yes" ] && n9997=$((n9997 + 1))
    total=$((total + 1))
    if [ "$total" -le 8 ]; then
        printf '%-8s %-8s %-6s %-6s %s\n' "$p" "$uid" "$has1023" "$has9997" "$g"
    fi
done

echo
echo "всего процессов приложений: $total"
echo "  с группой 1023 (media_rw):  $n1023"
echo "  с группой 9997 (everybody): $n9997"

echo
echo "--- для сравнения: группы системных и служебных процессов ---"
for p in $(ps -A -o PID,NAME | grep -E 'zygote|mediaprovider|vold|installd' | awk '{print $1}' | head -6); do
    [ -r "/proc/$p/status" ] || continue
    echo "pid=$p $(awk '/^Name:/{print $2}' /proc/$p/status) groups: $(awk '/^Groups:/{for (i=2;i<=NF;i++) printf \"%s \", $i}' /proc/$p/status)"
done
