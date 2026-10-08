#!/system/bin/sh
# final-verify.sh — final on-device check: (A) libc hook self-test (10 sections,
# ~45 assertions); (B) end-to-end from a live app's namespace; (C) storage-fix
# zeroing OTHER; (D) BOTH vold patches — state on the live process AND the
# resolution/emission self-tests. Requires hookselftest, device-e2e.sh,
# test-storage-fix.sh and vold-selftest.sh at /data/local/tmp.

T=/data/local/tmp/hookselftest
M=/data/adb/modules/unfuse_zygisk

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
echo "=== D. Патчи vold ==="
# Both patchers, same code set: 0 patched, 1 trampoline intact (no patch),
# 2 could not parse the target, 3 could not write, 4 no vold found.
for pair in "vold-noacl:setxattr" "vold-fusefs:mount+umount2"; do
    tool="${pair%%:*}"
    what="${pair#*:}"
    "$M/tools/$tool" --check >/dev/null 2>&1
    rc=$?
    case "$rc" in
        0) echo "  $what: на месте" ;;
        1) echo "  $what: НЕТ — трамплин цел" ;;
        2) echo "  $what: не удалось разобрать vold (код 2)" ;;
        3) echo "  $what: не удалось записать (код 3)" ;;
        *) echo "  $what: vold не найден (код $rc)" ;;
    esac
done

echo
echo "--- разбор и эмиссия (--selftest) ---"
if [ -x /data/local/tmp/vold-selftest.sh ]; then
    sh /data/local/tmp/vold-selftest.sh "$M/tools/vold-noacl" "$M/tools/vold-fusefs"
else
    echo "  нет /data/local/tmp/vold-selftest.sh — сначала:"
    echo "  adb push tools/vold-selftest.sh /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/vold-selftest.sh"
fi
echo
echo "инвариант /data/media/0 (ожидается ОК):"
"$M/tools/storage-fix" --check /data/media/0
rc=$?
if [ "$rc" = 0 ]; then
    echo "  ОК"
else
    echo "  НЕТ (код $rc)"
fi
echo
echo "default-ACL /data/media/0 (ожидается GROUP ... id=9997):"
if [ -x /data/local/tmp/acl-dump ]; then
    /data/local/tmp/acl-dump /data/media/0 | sed 's/^/  /'
else
    echo "  нет /data/local/tmp/acl-dump — смотрите --check выше"
fi
