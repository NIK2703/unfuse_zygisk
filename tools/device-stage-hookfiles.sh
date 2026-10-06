#!/system/bin/sh
# Копирует целевые файлы хуков в /data/local/tmp/pull, чтобы их можно было
# забрать через adb pull (который работает от shell, а не от root).

D=/data/local/tmp/pull
mkdir -p "$D/zygisksu" "$D/state"
chmod 755 "$D" "$D/zygisksu" "$D/state"

cp_one() {
    src="$1"; dst="$2"
    if [ -f "$src" ]; then
        cp -f "$src" "$dst" && chmod 644 "$dst" && echo "ok   $src"
    else
        echo "MISS $src"
    fi
}

cp_one /data/adb/modules/zygisksu/lib64/libzygisk.so     "$D/zygisksu/libzygisk64.so"
cp_one /data/adb/modules/zygisksu/lib64/libzn_loader.so  "$D/zygisksu/libzn_loader64.so"
cp_one /data/adb/modules/zygisksu/lib64/libpayload.so    "$D/zygisksu/libpayload64.so"
cp_one /data/adb/modules/zygisksu/lib/libzygisk.so       "$D/zygisksu/libzygisk32.so"
cp_one /data/adb/modules/zygisksu/lib/libzn_loader.so    "$D/zygisksu/libzn_loader32.so"
cp_one /data/adb/modules/zygisksu/bin/zygiskd64          "$D/zygisksu/zygiskd64"
cp_one /data/adb/modules/zygisksu/bin/zygiskd32          "$D/zygisksu/zygiskd32"
cp_one /data/adb/modules/zygisksu/module.prop            "$D/zygisksu/module.prop"
cp_one /data/adb/modules/zygisksu/post-fs-data.sh        "$D/zygisksu/post-fs-data.sh"
cp_one /data/adb/modules/zygisksu/service.sh             "$D/zygisksu/service.sh"
cp_one /data/adb/modules/zygisksu/sepolicy.rule          "$D/zygisksu/sepolicy.rule"
cp_one /data/adb/zygisksu/modules_info                   "$D/state/modules_info"
cp_one /data/adb/zygisksu/znctx                          "$D/state/znctx"
cp_one /data/adb/zygisksu/.magic                         "$D/state/magic"

# карты памяти zygote64 — какие хуки реально подгружены
ZY=$(pidof zygote64 2>/dev/null)
[ -n "$ZY" ] && grep -i -E 'zygisk|payload' /proc/$ZY/maps 2>/dev/null > "$D/state/zygote64.maps"
chmod 644 "$D/state/"* 2>/dev/null

echo "--- готово ---"
ls -la "$D/zygisksu" "$D/state"
