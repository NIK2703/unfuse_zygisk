#!/system/bin/sh
# Зонд 5: группы реальных процессов приложений + метки текущего FUSE-хранилища.
# Запуск:  su -c /data/local/tmp/probe5.sh

echo "===== probe5: $(date) ====="

echo
echo "--- группы процессов приложений ---"
for p in $(pidof com.android.systemui com.miui.home com.android.settings com.miui.gallery 2>/dev/null | tr ' ' '\n' | head -6); do
    name=$(cat /proc/$p/cmdline 2>/dev/null | tr '\0' ' ')
    echo "  pid=$p  $name"
    grep -E '^(Uid|Gid|Groups)' /proc/$p/status 2>/dev/null | sed 's/^/      /'
done

echo
echo "--- есть ли 9997 в группах хоть у кого-то ---"
found=0
for p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$' | head -400); do
    g=$(grep '^Groups:' /proc/$p/status 2>/dev/null)
    case "$g" in
        *9997*) 
            echo "  pid=$p $(cat /proc/$p/cmdline 2>/dev/null | tr '\0' ' ')"
            echo "      $g"
            found=$((found+1))
            [ "$found" -ge 5 ] && break
            ;;
    esac
done
[ "$found" = "0" ] && echo "  9997 НЕ найден ни у одного процесса (проверьте computeGidsForProcess)"

echo
echo "--- группы, которые ставит su 2000 ---"
su 2000 -c id 2>&1 | sed 's/^/      /'
su 2000 -c 'cat /proc/self/status | grep -E "^(Uid|Gid|Groups)"' 2>&1 | sed 's/^/      /'

echo
echo "--- метки текущего (FUSE) хранилища ---"
for p in /storage /storage/emulated /storage/emulated/0 /mnt/user/0 /mnt/user/0/emulated /mnt/pass_through/0/emulated; do
    printf '  %-34s ' "$p"
    ls -Zd "$p" 2>&1 | awk '{print $1, $3, $4}'
done

echo
echo "--- режимы текущего хранилища ---"
for p in /storage/emulated/0 /storage/emulated/0/DCIM /storage/emulated/0/Download; do
    printf '  %-34s ' "$p"
    stat -c '%a %u:%g' "$p" 2>&1
done

echo
echo "--- ACL на /data/media (следы unfuse_auto) ---"
getfacl /data/media 2>&1 | head -12 | sed 's/^/      /'
getfacl /data/media/0 2>&1 | head -12 | sed 's/^/      /'

echo
echo "--- unfuse_auto: uninstall.sh ---"
cat /data/adb/modules/unfuse_auto/uninstall.sh 2>&1 | head -40 | sed 's/^/      /'

echo
echo "--- app.te: правила про sdcard_type ---"
grep -n 'sdcard_type' /system/etc/selinux/plat_sepolicy.cil 2>/dev/null | grep -i 'appdomain\|untrusted_app' | head -10

echo
echo "===== конец ====="
