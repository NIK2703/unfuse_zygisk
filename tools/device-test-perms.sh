#!/system/bin/sh
# Проверка sdcardfs с ПРАВИЛЬНЫМ источником (/data/media) и с проверкой
# прав от лица приложения (uid 10123) и shell (2000).
#
# Всё в /data/local/tmp, после проверки снимается.

SRC=/data/media
BASE=/data/local/tmp/sdtest
APPUID=10123

echo "===== 0. ПОДГОТОВКА ====="
echo "--- /data/system/packages.list ---"
ls -la /data/system/packages.list 2>&1
wc -l /data/system/packages.list 2>&1
echo "--- примеры пакетов ---"
head -3 /data/system/packages.list 2>&1
echo "--- /data/media ---"
stat -c '%A %a %u %g %n' /data/media /data/media/0 2>&1

rm -rf "$BASE"
mkdir -p "$BASE"
chmod 755 "$BASE"
chmod 755 /data/local/tmp

case_run() {
    name="$1"; mask="$2"; gid="$3"
    dst="$BASE/$name"
    mkdir -p "$dst"; chmod 755 "$dst"

    echo
    echo "############ CASE $name  mask=$mask gid=$gid ############"
    mount -t sdcardfs \
        -o "fsuid=1023,fsgid=1023,multiuser,derive_gid,default_normal,unshared_obb,mask=$mask,userid=0,gid=$gid" \
        "$SRC" "$dst" || { echo "MOUNT FAILED"; return; }

    echo "--- mounts ---"
    grep "$BASE/$name " /proc/mounts

    echo "--- f_type ---"
    stat -f "$dst" 2>&1 | grep -E 'Type'

    echo "--- корень и пользовательский уровень ---"
    stat -c '%A %a %u %g %n' "$dst" "$dst/0" 2>&1

    echo "--- общие каталоги ---"
    for d in Download DCIM Pictures Android Android/data Android/media Android/obb; do
        if [ -e "$dst/0/$d" ]; then
            stat -c '%A %a %u %g %n' "$dst/0/$d"
        else
            echo "  (нет) $d"
        fi
    done

    echo "--- Android/data: чужие пакеты (первые 6) ---"
    ls -la "$dst/0/Android/data" 2>&1 | sed -n '3,9p'
    echo "--- права одного чужого пакета ---"
    other=$(ls "$dst/0/Android/data" 2>/dev/null | head -1)
    if [ -n "$other" ]; then
        stat -c '%A %a %u %g %n' "$dst/0/Android/data/$other" 2>&1
    fi

    echo "--- реальный файл в Download ---"
    f=$(ls "$dst/0/Download" 2>/dev/null | head -1)
    if [ -n "$f" ]; then
        stat -c '%A %a %u %g %s %n' "$dst/0/Download/$f"
    fi

    echo "--- ОТ ЛИЦА ROOT ---"
    ls "$dst/0/Download" >/dev/null 2>&1 && echo "  root read: OK" || echo "  root read: FAIL"

    echo "--- ОТ ЛИЦА ПРИЛОЖЕНИЯ (uid $APPUID) ---"
    su $APPUID -c "ls '$dst/0' >/dev/null 2>&1" && echo "  list /: OK" || echo "  list /: FAIL"
    su $APPUID -c "ls '$dst/0/Download' >/dev/null 2>&1" && echo "  list Download: OK" || echo "  list Download: FAIL"
    su $APPUID -c "touch '$dst/0/Download/.probe_$name' 2>/dev/null" && { echo "  write Download: OK"; rm -f "$dst/0/Download/.probe_$name"; } || echo "  write Download: FAIL"
    su $APPUID -c "ls '$dst/0/Android/data' >/dev/null 2>&1" && echo "  list Android/data: OK" || echo "  list Android/data: FAIL"
    su $APPUID -c "ls '$dst/0/Android/obb' >/dev/null 2>&1" && echo "  list Android/obb: OK" || echo "  list Android/obb: FAIL"
    if [ -n "$other" ]; then
        su $APPUID -c "ls '$dst/0/Android/data/$other' >/dev/null 2>&1" && echo "  чужой Android/data читается: ДА" || echo "  чужой Android/data читается: НЕТ"
    fi

    echo "--- ОТ ЛИЦА SHELL (2000) ---"
    su 2000 -c "ls '$dst/0/Download' >/dev/null 2>&1" && echo "  list Download: OK" || echo "  list Download: FAIL"

    echo "--- unmount ---"
    umount "$dst" && echo "  unmounted" || echo "  UMOUNT FAILED"
}

case_run default 6 1015
case_run read    27 9997
case_run write   7 9997
case_run full    7 9997

echo
echo "===== УБОРКА ====="
rm -rf "$BASE"
grep -c "$BASE" /proc/mounts 2>/dev/null
echo "===== КОНЕЦ ====="
