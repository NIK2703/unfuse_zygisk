#!/system/bin/sh
# Live test of variant B: bring up sdcardfs; baseline FUSE; bind sdcardfs over /mnt/user/0/emulated and /mnt/user/0 -> /storage; rollback.

SRC=/mnt/runtime/full/emulated
DST=/mnt/user/0/emulated
INNER=/data/local/tmp/inner.sh
NSPROBE=/data/local/tmp/nsprobe
RUNAS=/data/local/tmp/runas

APPUID=10465
APPGIDS="10465,20465,9997,3003"

echo "########## 1. ПОДНЯТЬ SDCARDFS ##########"
chmod 755 "$INNER" "$NSPROBE" "$RUNAS" /data/local/tmp 2>/dev/null
sh /data/local/tmp/bringup.sh

echo
echo "--- итоговые маунты ---"
grep -E 'mnt/runtime' /proc/mounts

echo
echo "########## 2. BASELINE (FUSE, без подмены) ##########"
"$RUNAS" "$APPUID" "$APPUID" "$APPGIDS" "$INNER" 2>&1 | sed 's/^/  /'

echo
echo "########## 3. ВАРИАНТ B (sdcardfs в приватном namespace) ##########"
"$NSPROBE" "$SRC" "$DST" "$APPUID" "$APPUID" "$APPGIDS" "$INNER" 2>&1 | sed 's/^/  /'

echo
echo "########## 4. ОТКАТ ##########"
for d in default read write full; do
    p=/mnt/runtime/$d/emulated
    umount "$p" 2>/dev/null && echo "  umounted $p"
done
grep -E 'mnt/runtime' /proc/mounts || echo "  чисто"

echo
echo "########## 5. СОСТОЯНИЕ /storage В ГЛОБАЛЬНОМ NS ##########"
grep -E '/storage' /proc/mounts

echo
echo "########## КОНЕЦ ##########"
