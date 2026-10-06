#!/system/bin/sh
# Diagnose why vold-noacl can't find the boot base.
T=/data/local/tmp/vold-noacl-new
P=$(for d in /proc/[0-9]*; do
        p=${d#/proc/}
        [ "$(readlink $d/exe 2>/dev/null)" = "/system/bin/vold" ] && echo "$p" && break
    done)
echo "vold pid=$P"

echo "--- читается ли /proc/$P/maps ---"
if head -1 /proc/$P/maps >/dev/null 2>&1; then
    echo "  да, строк: $(wc -l < /proc/$P/maps)"
    echo "  первые три записи с offset=0 и путём:"
    awk '$3 ~ /^[0-9a-f]+$/ && $3 == "00000000" && $6 ~ /^\// {print "   ", $1, $3, $6}' /proc/$P/maps | head -3
    echo "  минимальная из них (её и берёт утилита):"
    awk '$3 == "00000000" && $6 ~ /^\// {print $1, $6}' /proc/$P/maps | sort | head -1
else
    echo "  НЕТ: $(head -1 /proc/$P/maps 2>&1)"
fi

echo "--- 5 прогонов --dry-run ---"
i=0
while [ $i -lt 5 ]; do
    "$T" --dry-run >/tmp/dr 2>&1
    echo "  rc=$? : $(head -1 /tmp/dr)"
    i=$((i+1))
done

echo "--- 5 прогонов --check ---"
i=0
while [ $i -lt 5 ]; do
    "$T" --check >/tmp/dc 2>&1
    echo "  rc=$? : $(head -1 /tmp/dc)"
    i=$((i+1))
done
