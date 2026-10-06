#!/system/bin/sh
# Зонд 3: состояние unfuse_auto, ядра и ZygiskNext.
# Запуск:  su -c /data/local/tmp/probe3.sh

echo "===== probe3: $(date) ====="

echo
echo "--- unfuse_auto: содержимое ---"
ls -la /data/adb/modules/unfuse_auto/ 2>&1

echo
echo "--- unfuse_auto: marker disable / remove ---"
for m in disable remove update skip_mount; do
    [ -e "/data/adb/modules/unfuse_auto/$m" ] && echo "  ЕСТЬ: $m" || echo "  нет:  $m"
done

echo
echo "--- unfuse_auto: все файлы (кроме бинарников) ---"
find /data/adb/modules/unfuse_auto -maxdepth 2 -type f 2>/dev/null | head -40

echo
echo "--- unfuse_auto: логи ---"
for f in /data/adb/unfuse*log /data/adb/unfuse_auto* /data/adb/modules/unfuse_auto/*.log; do
    [ -f "$f" ] && { echo "  === $f ==="; tail -60 "$f" | sed 's/^/      /'; }
done
ls -la /data/adb/ 2>&1 | head -40

echo
echo "--- unfuse_auto: post-fs-data.sh полностью ---"
cat /data/adb/modules/unfuse_auto/post-fs-data.sh 2>&1

echo
echo "--- unfuse_auto: sepolicy.rule ---"
cat /data/adb/modules/unfuse_auto/sepolicy.rule 2>&1

echo
echo "--- ядро ---"
uname -a
echo "  build time: $(cat /proc/version)"

echo
echo "--- splice в kallsyms ---"
grep -i splice /proc/kallsyms | head -20
echo -n "  sdcardfs_splice_read: "
grep -c sdcardfs_splice_read /proc/kallsyms

echo
echo "--- наши модули уже стоят? ---"
ls -la /data/adb/modules/sdcardfs_restore 2>&1
ls -la /data/adb/sdcardfs-apps.conf 2>&1

echo
echo "--- ZygiskNext ---"
cat /data/adb/modules/zygisksu/module.prop 2>&1
ls -la /data/adb/zygisksu/ 2>&1

echo
echo "--- resetprop есть? ---"
which resetprop 2>&1
resetprop --help 2>&1 | head -5

echo
echo "===== конец ====="
