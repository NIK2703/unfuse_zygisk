#!/system/bin/sh
# Why stat("/mnt/runtime/full/emulated") failed inside preAppSpecialize.

echo "########## 1. есть ли /mnt/runtime в namespace zygote ##########"
ZY=$(pidof zygote64 2>/dev/null | awk '{print $1}')
APP=$(pidof com.mixplorer 2>/dev/null | awk '{print $1}')
echo "zygote64 pid=$ZY   app pid=${APP:-нет}"
echo "--- ns/mnt ---"
echo "  zygote: $(readlink /proc/$ZY/ns/mnt 2>&1)"
echo "  init:   $(readlink /proc/1/ns/mnt 2>&1)"
[ -n "$APP" ] && echo "  app:    $(readlink /proc/$APP/ns/mnt 2>&1)"

echo
echo "--- /mnt/runtime в namespace ZYGOTE ---"
grep -E "mnt/runtime" /proc/$ZY/mounts 2>&1 | sed 's/^/  /'
echo "  (если пусто — zygote не видит /mnt/runtime вообще)"

echo
echo "--- /mnt/runtime в namespace INIT ---"
grep -E "mnt/runtime" /proc/1/mounts 2>&1 | sed 's/^/  /'

echo
echo "--- доходит ли путь через /proc/PID/root ---"
for p in 1 $ZY; do
    printf "  pid %-6s ls /mnt/runtime/full/emulated: " "$p"
    ls /proc/$p/root/mnt/runtime/full/emulated 2>&1 | head -2 | tr '\n' ' '
    echo
    printf "  pid %-6s stat -f: " "$p"
    stat -f /proc/$p/root/mnt/runtime/full/emulated 2>&1 | grep -o "Type:.*"
done

echo
echo "########## 2. метки пути ##########"
for p in /mnt /mnt/runtime /mnt/runtime/full /mnt/runtime/full/emulated \
         /mnt/runtime/full/emulated/0 /mnt/user/0/emulated ; do
    printf "  %-36s " "$p"; stat -c "%C" "$p" 2>&1
done

echo
echo "########## 3. propagation: shared/private ##########"
cat /proc/1/mountinfo 2>/dev/null | grep -E " / | /mnt | /mnt/runtime " | head -10

echo
echo "########## 4. AVC-отказы (logcat, все буферы) ##########"
logcat -b all -d 2>/dev/null | grep -i "avc:" | tail -30
echo "  всего avc в logcat: $(logcat -b all -d 2>/dev/null | grep -ci 'avc:')"
echo
echo "--- avc в dmesg ---"
dmesg 2>/dev/null | grep -i "avc:" | tail -20
echo "  всего avc в dmesg: $(dmesg 2>/dev/null | grep -ci 'avc:')"

echo
echo "########## 5. есть ли nsenter ##########"
command -v nsenter && nsenter --help 2>&1 | head -5 || echo "  nsenter нет"

echo
echo "########## конец ##########"
