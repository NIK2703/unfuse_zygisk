#!/system/bin/sh
# final-verify.sh — final on-device check: (A) libc hook self-test (28 checks);
# (B) end-to-end from a live app's namespace; (C) storage-fix zeroing OTHER;
# (D) vold patch state. Requires hookselftest and device-e2e.sh at /data/local/tmp.

T=/data/local/tmp/hookselftest
M=/data/adb/modules/unfuse_zygisk
LOG=/data/adb/unfuse_zygisk.log

if [ ! -x "$T" ]; then
    echo "нет $T — сначала: adb push out/hookselftest-arm64 $T && adb shell chmod 755 $T"
    exit 1
fi

echo "=== A. Самопроверка хука libc ==="
"$T" | tail -3

echo
echo "=== B. Сквозная проверка из namespace приложения ==="
sh /data/local/tmp/device-e2e.sh 2>&1 | tail -8

echo
echo "=== C. Обнуление «остальных» в ACL (storage-fix) ==="
sh /data/local/tmp/test-storage-fix.sh 2>&1 | grep -E '^/data.*OTHER|открывает' | head -6

echo
echo "=== D. Патч vold ==="
# vold-noacl codes: 0 patched, 1 trampoline intact (no patch), 2 parse fail, 3 write fail.
"$M/tools/vold-noacl" --check >/dev/null 2>&1
rc=$?
case "$rc" in
    0) echo "патч: на месте" ;;
    1) echo "патч: НЕТ — трамплин setxattr цел" ;;
    2) echo "патч: не удалось разобрать vold (код 2)" ;;
    3) echo "патч: не удалось записать (код 3)" ;;
    *) echo "патч: vold не найден (код $rc)" ;;
esac
echo "журнал патча:"
grep -E 'vold-noacl|патч vold' "$LOG" | tail -4 | sed 's/^/  /'
echo
echo "инвариант /data/media/0 (ожидается ОК):"
"$M/tools/storage-fix" --check /data/media/0 | sed 's/^/  /'
echo
echo "default-ACL /data/media/0 (ожидается GROUP ... id=9997):"
if [ -x /data/local/tmp/acl-dump ]; then
    /data/local/tmp/acl-dump /data/media/0 | sed 's/^/  /'
else
    echo "  нет /data/local/tmp/acl-dump — смотрите --check выше"
fi
