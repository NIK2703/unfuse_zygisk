#!/system/bin/sh
# bitness.sh — counts 32- vs 64-bit app processes; the libc hook is arm64-only, so 32-bit-created files keep kernel modes/ACL.

echo "--- зиготы ---"
ps -A -o PID,NAME | grep -i zygote

echo
echo "--- класс ELF процессов приложений (1 = 32 бита, 2 = 64 бита) ---"
n32=0
n64=0
for p in $(ps -A -o PID,UID | awk '$2 >= 10000 && $2 < 20000 {print $1}'); do
    exe="/proc/$p/exe"
    [ -r "$exe" ] || continue
    cls=$(od -An -tu1 -j4 -N1 "$exe" 2>/dev/null | tr -d ' ')
    name=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null)
    if [ "$cls" = "1" ]; then
        n32=$((n32 + 1))
        echo "  32 бита: pid=$p $name"
    elif [ "$cls" = "2" ]; then
        n64=$((n64 + 1))
    fi
done

echo
echo "итого: 32-битных=$n32  64-битных=$n64"

echo
echo "--- какие установленные пакеты только 32-битные ---"
pm list packages -3 2>/dev/null | sed 's/^package://' | while read -r pkg; do
    abi=$(dumpsys package "$pkg" 2>/dev/null | grep -m1 primaryCpuAbi | tr -d ' ')
    case "$abi" in
        *armeabi*|*x86:*) echo "  $pkg -> $abi" ;;
    esac
done | head -20
