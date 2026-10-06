#!/system/bin/sh
echo "===== есть ли nsenter ====="
command -v nsenter 2>&1

echo
echo "===== режимы, которые отдаёт драйвер sdcardfs ====="
for p in /mnt/runtime/full/emulated \
         /mnt/runtime/full/emulated/0 \
         /mnt/runtime/full/emulated/0/Download \
         /mnt/runtime/full/emulated/0/Android \
         /mnt/runtime/full/emulated/0/Android/data; do
    stat -c '  %n mode=%a uid=%u gid=%g ctx=%C' "$p" 2>&1
done

echo
echo "===== файлы (должно быть 660) ====="
f=$(ls /mnt/runtime/full/emulated/0/*.* 2>/dev/null | head -1)
[ -n "$f" ] && stat -c '  %n mode=%a gid=%g' "$f" 2>&1

echo
echo "===== перезапуск нескольких приложений ====="
for p in com.android.documentsui com.android.settings com.mixplorer com.aimp.player; do
    pm path "$p" >/dev/null 2>&1 || continue
    am force-stop "$p" 2>/dev/null
    monkey -p "$p" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
    echo "  запущено: $p"
done

sleep 6

echo
echo "===== logcat SdcardFsRestore ====="
logcat -d -s SdcardFsRestore 2>/dev/null | tail -40

echo
echo "===== конец ====="
