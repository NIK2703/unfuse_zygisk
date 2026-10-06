#!/system/bin/sh
# Живая проверка механизма варианта B на устройстве.
#
#  1. поднять sdcardfs на /mnt/runtime/*/emulated с правильными опциями
#  2. baseline: что видит приложение БЕЗ подмены (FUSE)
#  3. вариант B: приватный namespace + bind sdcardfs поверх /mnt/user/0/emulated
#     + bind /mnt/user/0 -> /storage (как Zygote)
#  4. откат

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
