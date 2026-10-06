#!/system/bin/sh
# Probe 4: does sdcardfs mount at all here, and what labels does the mount root get? Mounts into an empty /data/local/tmp dir, unmounts immediately.

T=/data/local/tmp/sdfstest

echo "===== probe4: $(date) ====="

echo
echo "--- предыдущее состояние unfuse_auto ---"
echo -n "  .backend     = "; cat /data/adb/modules/unfuse_auto/.backend 2>&1
echo -n "  .boot-tries  = "; cat /data/adb/modules/unfuse_auto/.boot-tries 2>&1
echo -n "  .acl-applied = "; cat /data/adb/modules/unfuse_auto/.acl-applied 2>&1
echo "  --- /data/adb/marble_unfuse_auto.conf ---"
sed 's/^/      /' /data/adb/marble_unfuse_auto.conf 2>&1

echo
echo "--- метки нижнего файлового дерева ---"
for p in /data/media /data/media/0 /data/media/0/DCIM /data/media/0/Android \
         /data/media/0/Android/data /data/media/0/Android/obb /data/media/obb; do
    printf '  %-36s ' "$p"
    ls -Zd "$p" 2>&1 | awk '{print $1, $3, $4}'
done

echo
echo "--- попытка смонтировать sdcardfs в scratch-каталог ---"
umount -l "$T" 2>/dev/null
rmdir "$T" 2>/dev/null
mkdir -p "$T" || { echo "  не удалось создать $T"; exit 1; }
chmod 755 "$T"

OPTS="fsuid=1023,fsgid=1023,multiuser,derive_gid,mask=0007,userid=0,gid=9997"
echo "  mount -t sdcardfs -o $OPTS /data/media $T"
if mount -t sdcardfs -o "$OPTS" /data/media "$T" 2>&1; then
    echo "  РЕЗУЛЬТАТ: смонтировано"
    RC=0
else
    echo "  РЕЗУЛЬТАТ: mount не удался (rc=$?)"
    RC=1
fi

if [ "$RC" = "0" ]; then
    echo
    echo "  --- /proc/mounts (наша строка) ---"
    grep "$T" /proc/mounts | sed 's/^/      /'

    echo
    echo "  --- ls -la \$T ---"
    ls -la "$T" 2>&1 | head -15 | sed 's/^/      /'

    echo
    echo "  --- ls -Zla \$T (метки) ---"
    ls -Zla "$T" 2>&1 | head -15 | sed 's/^/      /'

    echo
    echo "  --- ls -Zla \$T/0 ---"
    ls -Zla "$T/0" 2>&1 | head -15 | sed 's/^/      /'

    echo
    echo "  --- stat -c %a:%u:%g \$T / \$T/0 / \$T/0/DCIM ---"
    for p in "$T" "$T/0" "$T/0/DCIM" "$T/0/Android"; do
        printf '      %-24s ' "$p"
        stat -c '%a %u:%g' "$p" 2>&1
    done

    echo
    echo "  --- stat -f \$T (тип ФС) ---"
    stat -f "$T" 2>&1 | sed 's/^/      /'

    echo
    echo "  --- чтение как shell (uid 2000) ---"
    su 2000 -c "ls -la $T" 2>&1 | head -5 | sed 's/^/      /'
    su 2000 -c "ls -la $T/0" 2>&1 | head -5 | sed 's/^/      /'

    echo
    echo "  --- отмонтирование ---"
    umount "$T" 2>&1 && echo "      отмонтировано" || { echo "      umount не удался, пробую -l"; umount -l "$T" 2>&1; }
else
    echo
    echo "  --- dmesg вокруг попытки ---"
    dmesg 2>/dev/null | tail -15 | sed 's/^/      /'
fi

rmdir "$T" 2>/dev/null

echo
echo "===== конец ====="
