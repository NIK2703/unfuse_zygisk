#!/system/bin/sh
# Эмпирическая проверка драйвера sdcardfs на устройстве.
# Всё делается в /data/local/tmp, после проверки маунт снимается.
# Ничего в системе не меняется постоянно.

SRC=/data/media/0
BASE=/data/local/tmp/sdtest

echo "===== 0. ПОДГОТОВКА ====="
mkdir -p "$BASE" || { echo "не создать $BASE"; exit 1; }
ls -lad "$BASE"
echo "--- source: ---"
stat -c '%A %u %g %n' "$SRC"

run_case() {
    name="$1"; mask="$2"; gid="$3"; extra="$4"
    dst="$BASE/$name"
    mkdir -p "$dst"

    echo
    echo "===== CASE $name: mask=$mask gid=$gid extra=[$extra] ====="
    if ! mount -t sdcardfs -o "fsuid=1023,fsgid=1023,multiuser,derive_gid,default_normal,unshared_obb,mask=$mask,userid=0,gid=$gid" "$SRC" "$dst" 2>&1; then
        echo "MOUNT FAILED (с полным набором опций)"
        if ! mount -t sdcardfs -o "fsuid=1023,fsgid=1023,multiuser,derive_gid,mask=$mask,userid=0,gid=$gid" "$SRC" "$dst" 2>&1; then
            echo "MOUNT FAILED (без default_normal/unshared_obb) — драйвер не принимает опции"
            rmdir "$dst" 2>/dev/null
            return
        fi
        echo "(смонтировалось без default_normal/unshared_obb)"
    fi

    echo "--- /proc/mounts ---"
    grep "$name" /proc/mounts

    echo "--- statfs (f_type) ---"
    stat -f "$dst" 2>&1 | grep -E 'Type|ID'

    echo "--- права верхнего уровня ---"
    stat -c '%A %a %u %g %n' "$dst" "$dst/0" 2>&1

    echo "--- права на каталогах внутри ---"
    for d in Download DCIM Android Android/data Android/media Android/obb; do
        if [ -e "$dst/0/$d" ]; then
            stat -c '%A %a %u %g %n' "$dst/0/$d"
        else
            echo "  (нет) $d"
        fi
    done

    echo "--- Android/data: каталоги пакетов ---"
    ls -la "$dst/0/Android/data" 2>&1 | head -8

    echo "--- чтение реального файла ---"
    f=$(ls "$dst/0/Download" 2>/dev/null | head -1)
    if [ -n "$f" ]; then
        echo "файл: $f"
        stat -c '%A %a %u %g %s %n' "$dst/0/Download/$f"
        head -c 16 "$dst/0/Download/$f" >/dev/null 2>&1 && echo "READ OK" || echo "READ FAIL"
    fi

    echo "--- unmount ---"
    umount "$dst" && echo "unmounted" || echo "UMOUNT FAILED"
    grep "$name" /proc/mounts && echo "!!! всё ещё смонтирован"
    rmdir "$dst" 2>/dev/null
}

run_case default 6 1015
run_case read    27 9997
run_case write   7 9997
run_case full    7 9997

echo
echo "===== ПРОВЕРКА ПРАВ ОТ ЛИЦА ПРИЛОЖЕНИЯ ====="
echo "--- умеет ли su менять uid ---"
su 10123 -c id 2>&1 | head -2
echo "--- su 2000 (shell) ---"
su 2000 -c id 2>&1 | head -2

echo
echo "===== ОСТАТКИ ====="
grep sdtest /proc/mounts || echo "чисто"
ls -la "$BASE" 2>&1
rmdir "$BASE" 2>/dev/null

echo
echo "===== КОНЕЦ ====="
