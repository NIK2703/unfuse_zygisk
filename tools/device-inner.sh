#!/system/bin/sh
# Внутренний скрипт: выполняется от лица процесса приложения
# (uid/gid/группы как у реального приложения). Печатает то, что видно
# приложению в /storage/emulated/0.

echo "id: $(id)"
echo "mounts:"
grep -E 'mnt/user/0/emulated|/storage' /proc/mounts | sed 's/^/    /'
echo "statfs /storage/emulated/0:"
stat -f /storage/emulated/0 2>&1 | grep -E 'Type|ID' | sed 's/^/    /'

echo "checks:"
ck() {
    desc="$1"; shift
    if "$@" >/dev/null 2>&1; then echo "    OK   $desc"; else echo "    DENY $desc"; fi
}
ck "list /storage/emulated/0"        ls /storage/emulated/0
ck "list Download"                   ls /storage/emulated/0/Download
ck "read файл из Download"           head -c 16 /storage/emulated/0/Download
ck "WRITE в Download"                touch /storage/emulated/0/Download/.probe_ns
ck "list Android"                    ls /storage/emulated/0/Android
ck "list Android/data"               ls /storage/emulated/0/Android/data
ck "list Android/obb"                ls /storage/emulated/0/Android/obb
ck "list Android/media"              ls /storage/emulated/0/Android/media
ck "СВОЙ Android/data"               ls /storage/emulated/0/Android/data/ai.qwenlm.chat.android
ck "ЧУЖОЙ Android/data (aimp)"       ls /storage/emulated/0/Android/data/com.aimp.player

rm -f /storage/emulated/0/Download/.probe_ns 2>/dev/null
