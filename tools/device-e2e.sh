#!/system/bin/sh
# device-e2e.sh — end-to-end check from a REAL app's namespace (as root): (1)
# /storage/emulated/0 serves the raw tree, not FUSE; (2) the libc hook works on
# the app's own /storage paths.
#
# The root namespace is NOT the control here. The module's bind sits on
# /mnt/user/<u>/emulated, a shared mount, so it propagates to init's namespace
# too and both sides read f2fs. That was once the contrast this script drew
# ("root has FUSE, the app has the raw tree"); it no longer holds, and neither
# does the FUSE-side readas below — both now succeed. What still matters is that
# the app namespace is raw AND that a foreign uid can read what the hook shaped.

T=/data/local/tmp/hookselftest

echo "=== 1. Вид файловой системы: корневой namespace против namespace приложения ==="

PID=""
for p in $(ps -A -o PID,UID,NAME | awk '$2 >= 10000 && $2 < 20000 {print $1}'); do
    if [ -r "/proc/$p/ns/mnt" ]; then PID="$p"; break; fi
done

if [ -z "$PID" ]; then
    echo "нет живого процесса приложения — запустите любое приложение и повторите"
    exit 1
fi

UID_APP=$(ps -o UID= -p "$PID" | tr -d ' ')
NAME_APP=$(ps -o NAME= -p "$PID" | tr -d ' ')
echo "процесс приложения: pid=$PID uid=$UID_APP name=$NAME_APP"
echo

echo "--- корневой namespace (bind распространяется сюда же: тоже сырое дерево) ---"
"$T" fs /storage/emulated/0 /storage/emulated/0/Download /data/media/0

echo
echo "--- namespace приложения (ожидается f2fs — сырое дерево) ---"
nsenter --mount="/proc/$PID/ns/mnt" "$T" fs \
    /storage/emulated/0 /storage/emulated/0/Download /data/media/0

echo
echo "=== 2. Тот же каталог, вид с обеих сторон ==="
echo "--- корневой namespace ---"
"$T" acl /storage/emulated/0/Download
echo "--- namespace приложения ---"
nsenter --mount="/proc/$PID/ns/mnt" "$T" acl /storage/emulated/0/Download

echo
echo "=== 3. Создание файла из namespace приложения, от uid приложения ==="
# Created by the hooking process itself (toybox-nsenter lacks --setuid, so the tool
# drops privileges). Mode 0600 zeroed the ACL mask on the raw tree, shutting others out.
nsenter --mount="/proc/$PID/ns/mnt" "$T" writeas "$UID_APP" \
    /storage/emulated/0/Download/.e2e_appwrite 0600

echo
echo "--- каким файл видит корневой namespace (снаружи) ---"
"$T" acl /storage/emulated/0/Download/.e2e_appwrite

echo
echo "--- доступен ли он ДРУГОМУ uid (10998 вместо $UID_APP) ---"
# Both sides read it now that the bind reaches the root namespace too, so this is
# a positive check only: the hook's 0660 + 9997 ACL is what lets a foreign uid in.
nsenter --mount="/proc/$PID/ns/mnt" "$T" readas 10998 \
    /storage/emulated/0/Download/.e2e_appwrite

echo "--- тот же uid в корневом namespace (раньше падал на FUSE, теперь тоже читает) ---"
"$T" readas 10998 /storage/emulated/0/Download/.e2e_appwrite 2>&1 || true

rm -f /data/media/0/Download/.e2e_appwrite

echo
echo "=== 4. Полная проверка хука внутри namespace приложения ==="
nsenter --mount="/proc/$PID/ns/mnt" "$T"

echo
echo "=== 5. Изоляция Android/{data,obb} снята в namespace приложения ==="
# This is the module's headline and nothing else checks it: unfuse_zygisk.cpp
# tells zygote *args->mount_storage_dirs = JNI_FALSE, so no per-package tmpfs is
# put over Android/{data,obb} and the shared tree underneath stays visible.
#
# Two discriminators, both needed. A tmpfs has its own dev, so dev must equal
# /data/media/0's; and an isolated dir holds one package at most (the app's own),
# so the count must equal what the root namespace sees for THAT SAME dir — obb is
# legitimately shorter than data, so one shared number would be wrong.
#
# `--` before the command is required: toybox nsenter otherwise takes the -c of
# `stat -c %d` for its own option and dies with "Unknown option 'c'".

RAW_DEV=$(stat -c %d /data/media/0)
echo "  /data/media/0: dev=$RAW_DEV"

for p in /storage/emulated/0/Android/data /storage/emulated/0/Android/obb; do
    N_ROOT=$(ls "$p" 2>/dev/null | wc -l)
    D=$(nsenter --mount="/proc/$PID/ns/mnt" -- stat -c %d "$p" 2>/dev/null)
    N=$(nsenter --mount="/proc/$PID/ns/mnt" -- ls "$p" 2>/dev/null | wc -l)
    if [ "$D" = "$RAW_DEV" ] && [ "$N" = "$N_ROOT" ]; then
        echo "  [ок] $p: dev=$D (сырое дерево), записей $N"
    else
        echo "  [ПРОВАЛ] $p: dev=$D (ждали $RAW_DEV), записей $N (ждали $N_ROOT)"
    fi
done

echo
echo "готово"
