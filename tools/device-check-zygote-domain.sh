#!/system/bin/sh
# Can the zygote domain unshare(CLONE_NEWNS) and mount()? That's what preAppSpecialize does.

echo "########## 1. SELinux ##########"
echo -n "getenforce: "; getenforce 2>/dev/null
echo -n "enforce:    "; cat /sys/fs/selinux/enforce 2>/dev/null
echo -n "policycap:  "; cat /sys/fs/selinux/policy_capabilities/* 2>/dev/null | tr '\n' ' '; echo

echo
echo "########## 2. zygote64: домен и capabilities ##########"
ZY=$(pidof zygote64 2>/dev/null)
echo "zygote64 pid: ${ZY:-нет}"
if [ -n "$ZY" ]; then
    echo -n "domain: "; cat /proc/$ZY/attr/current 2>/dev/null
    echo -n "CapEff: "; grep -E '^CapEff' /proc/$ZY/status 2>/dev/null
    echo -n "CapPrm: "; grep -E '^CapPrm' /proc/$ZY/status 2>/dev/null
    echo -n "Uid:    "; grep -E '^Uid' /proc/$ZY/status 2>/dev/null
    echo -n "NoNewPrivs: "; grep -E '^NoNewPrivs' /proc/$ZY/status 2>/dev/null
    echo -n "Seccomp: "; grep -E '^Seccomp' /proc/$ZY/status 2>/dev/null
fi

echo
echo "########## 3. nsdaemon-процессы ZygiskNext ##########"
ps -A -o PID,USER,LABEL,NAME 2>/dev/null | grep -E 'zn-|zygote' | head -20

echo
echo "########## 4. домены nsdaemon ##########"
for p in $(pidof zn-nsdaemon-zygote 2>/dev/null) $(pidof zn-zygisk-companion64 2>/dev/null); do
    echo -n "pid $p: "; cat /proc/$p/attr/current 2>/dev/null
    echo -n "  CapEff: "; grep -E '^CapEff' /proc/$p/status 2>/dev/null
done

echo
echo "########## 5. пробуем unshare -m из домена zygote ##########"
# runcon may be denied — that is also a result.
echo -n "runcon -> zygote: "
runcon u:r:zygote:s0 /system/bin/sh -c 'echo запущено; id -Z 2>/dev/null' 2>&1 | head -3

echo
echo "########## 6. есть ли утилиты для анализа политики ##########"
for t in sesearch sepolicy-analyze seinfo magiskpolicy ksud; do
    p=$(command -v $t 2>/dev/null)
    [ -n "$p" ] && echo "OK   $t -> $p" || echo "нет  $t"
done
for p in /data/adb/ksud /data/adb/magisk/magiskpolicy /system/bin/magiskpolicy; do
    [ -e "$p" ] && echo "есть $p"
done

echo
echo "########## 7. ksud: есть ли команда sepolicy ##########"
/data/adb/ksud --help 2>&1 | head -40

echo
echo "########## конец ##########"
