#!/system/bin/sh
# Диагностика домена zygote. Пишем в файл, чтобы не терять вывод.
OUT=/data/local/tmp/zygote-domain.txt
: > "$OUT"
say() { echo "$@" >> "$OUT"; }

say "########## SELinux ##########"
say "getenforce: $(getenforce 2>&1)"
say "enforce:    $(cat /sys/fs/selinux/enforce 2>&1)"
say "policyvers: $(cat /sys/fs/selinux/policyvers 2>&1)"
say "mydomain:   $(id -Z 2>&1)"

say ""
say "########## zygote ##########"
for z in zygote64 zygote; do
    ZY=$(pidof $z 2>/dev/null)
    say "--- $z pid=${ZY:-нет} ---"
    [ -z "$ZY" ] && continue
    grep -E '^(Uid|Gid|CapEff|CapPrm|CapBnd|NoNewPrivs|Seccomp|Seccomp_filters):' \
        /proc/$ZY/status 2>&1 | sed 's/^/    /' >> "$OUT"
    say "    label: $(cat /proc/$ZY/attr/current 2>&1)"
done

say ""
say "########## ps с метками ##########"
ps -A -o PID,USER,LABEL,NAME 2>/dev/null | grep -E 'zn-|zygote|zygisk' | sed 's/^/    /' >> "$OUT"

say ""
say "########## nsdaemon / companion ##########"
for n in zn-nsdaemon-zygote zn-nsdaemon-zygote_secondary zn-zygisk-companion64; do
    for p in $(pidof $n 2>/dev/null); do
        say "--- $n pid=$p ---"
        say "    label:  $(cat /proc/$p/attr/current 2>&1)"
        grep -E '^(Uid|CapEff|CapPrm|NoNewPrivs|Seccomp):' /proc/$p/status 2>&1 | sed 's/^/    /' >> "$OUT"
    done
done

say ""
say "########## пробуем перейти в домен zygote ##########"
say "runcon u:r:zygote:s0 id -Z:"
runcon u:r:zygote:s0 /system/bin/sh -c 'id -Z' >> "$OUT" 2>&1

say ""
say "runcon u:r:zygote:s0 unshare -m:"
runcon u:r:zygote:s0 /system/bin/sh -c 'unshare -m /system/bin/true' >> "$OUT" 2>&1
say "  rc=$?"

say ""
say "########## инструменты анализа политики ##########"
for t in sesearch sepolicy-analyze seinfo magiskpolicy ksud getenforce runcon unshare; do
    p=$(command -v $t 2>/dev/null)
    say "  $t -> ${p:-НЕТ}"
done

say ""
say "########## ksud --help ##########"
/data/adb/ksud --help >> "$OUT" 2>&1

say ""
say "########## ksud sepolicy --help ##########"
/data/adb/ksud sepolicy --help >> "$OUT" 2>&1

chmod 644 "$OUT"
echo "готово: $OUT ($(wc -c < "$OUT") байт)"
