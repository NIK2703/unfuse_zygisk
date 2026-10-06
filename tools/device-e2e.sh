#!/system/bin/sh
# device-e2e.sh — end-to-end check from a REAL app's namespace (as root): (1)
# /storage/emulated/0 is the raw tree, not FUSE (root namespace has FUSE, so the
# comparison matters); (2) the libc hook works on the app's own /storage paths.

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

echo "--- корневой namespace (ожидается FUSE на /storage/emulated/0) ---"
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
# Must run IN the app namespace: root-namespace /storage/emulated is FUSE, where a
# foreign uid legitimately gets EFAULT; on the raw tree the same uid reads unimpeded.
nsenter --mount="/proc/$PID/ns/mnt" "$T" readas 10998 \
    /storage/emulated/0/Download/.e2e_appwrite

echo "--- для контраста: тот же uid через FUSE в корневом namespace ---"
"$T" readas 10998 /storage/emulated/0/Download/.e2e_appwrite 2>&1 || true

rm -f /data/media/0/Download/.e2e_appwrite

echo
echo "=== 4. Полная проверка хука внутри namespace приложения ==="
nsenter --mount="/proc/$PID/ns/mnt" "$T"

echo
echo "готово"
