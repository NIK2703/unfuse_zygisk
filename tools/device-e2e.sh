#!/system/bin/sh
#
# device-e2e.sh — сквозная проверка модуля из namespace НАСТОЯЩЕГО приложения.
#
# Запускается на устройстве от root. Находит живой процесс приложения, заходит в
# его mount namespace и проверяет две вещи:
#
#   1. Что /storage/emulated/0 отдаёт сырое дерево, а не FUSE — то есть что
#      подмена точки монтирования, которую делает модуль, действительно видна
#      приложению. В корневом namespace там FUSE, поэтому сравнение обязательно.
#
#   2. Что правка входов libc работает на тех путях, которыми пользуется
#      приложение (/storage/emulated/0/...), а не только на /data/media.
#
# Использование:  su -c sh /data/local/tmp/device-e2e.sh
#

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
# Файл создаётся процессом, который сам поставил хуки (у toybox-nsenter нет
# --setuid, поэтому сброс прав делает сам инструмент). Режим 0600 — тот самый
# случай, который на сыром дереве обнулял маску ACL и закрывал файл от чужих.
nsenter --mount="/proc/$PID/ns/mnt" "$T" writeas "$UID_APP" \
    /storage/emulated/0/Download/.e2e_appwrite 0600

echo
echo "--- каким файл видит корневой namespace (снаружи) ---"
"$T" acl /storage/emulated/0/Download/.e2e_appwrite

echo
echo "--- доступен ли он ДРУГОМУ uid (10998 вместо $UID_APP) ---"
#
# ВАЖНО: проверка делается В namespace приложения, и это не придирка.
# В корневом namespace /storage/emulated — это FUSE, и открытие обычного файла
# посторонним uid там законно падает с EFAULT (Bad address): демон
# MediaProvider не обслуживает uid, не принадлежащие этому пространству имён.
# Проверено на файле, которого модуль никогда не касался, — значит к модулю это
# отношения не имеет. Ради этого модуль и существует: в сыром дереве тот же uid
# читает чужой файл без всяких препятствий.
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
