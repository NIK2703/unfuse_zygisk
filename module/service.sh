# shellcheck shell=sh
#
# service.sh — запускается в late_start service, то есть уже после того, как
# vold смонтирует хранилища.
#
# Делает две вещи:
#   1. повторный (идемпотентный) bringup sdcardfs — на случай, если
#      post-fs-data отработал раньше, чем vold создал каталоги;
#   2. собирает диагностику в /data/adb/sdcardfs_restore.log.
#

LOG=/data/adb/sdcardfs_restore.log
MODDIR=${MODDIR:-${0%/*}}
CONF="$MODDIR/config"

# Ждём, пока vold действительно смонтирует эмуляцию (до ~20 секунд).
i=0
while [ "$i" -lt 40 ]; do
    [ -d /mnt/user/0/emulated ] && break
    sleep 0.5
    i=$((i + 1))
done

# Перемаркировка корня /data/media: vold при подготовке хранилища мог вернуть
# ярлык media_userdir_file, поэтому повторяем уже после старта vold.
if [ -f "$MODDIR/relabel-media.sh" ]; then
    MODDIR="$MODDIR" sh "$MODDIR/relabel-media.sh"
fi

# Права на /mnt/runtime и /data/media: vold мог вернуть их обратно после
# post-fs-data, поэтому повторяем уже после старта vold.
if [ -f "$MODDIR/relax-storage.sh" ]; then
    MODDIR="$MODDIR" sh "$MODDIR/relax-storage.sh"
fi

# Повторный bringup: страхует от гонки с vold и от ручного umount.
if [ -f "$MODDIR/sdcardfs-bringup.sh" ] && [ ! -f /data/adb/sdcardfs_restore.disable ]; then
    {
        echo "----- service: повторный bringup $(date) -----"
        sh "$MODDIR/sdcardfs-bringup.sh"
    } >> "$LOG" 2>&1
fi

{
    echo "===== sdcardfs_restore: диагностика $(date) ====="

    echo "--- свойства ---"
    echo "external_storage.sdcardfs.enabled = $(getprop external_storage.sdcardfs.enabled)"
    echo "ro.crypto.state                   = $(getprop ro.crypto.state)"
    echo "ro.build.version.release          = $(getprop ro.build.version.release)"
    echo "ro.build.version.sdk              = $(getprop ro.build.version.sdk)"

    echo "--- права на каталоги хранения ---"
    for p in /mnt/runtime /mnt/runtime/full /mnt/runtime/full/emulated \
             /data/media /data/media/0 /mnt/user /mnt/user/0 \
             /mnt/user/0/emulated; do
        echo "  $(stat -c '%n mode=%a uid=%u gid=%g' "$p" 2>&1)"
    done

    echo "--- ярлыки SELinux (корень хранилища должен быть media_rw_data_file) ---"
    for p in /data/media /data/media/0 /mnt/runtime/full/emulated \
             /mnt/runtime/full/emulated/0; do
        echo "  $(ls -Zd "$p" 2>&1)"
    done

    echo "--- точки монтирования ---"
    for p in /mnt/runtime/default/emulated \
             /mnt/runtime/read/emulated \
             /mnt/runtime/write/emulated \
             /mnt/runtime/full/emulated \
             /mnt/pass_through/0/emulated \
             /mnt/user/0/emulated \
             /mnt/user/0 \
             /storage; do
        line=$(grep -w "$p" /proc/mounts 2>/dev/null | head -1)
        if [ -n "$line" ]; then
            echo "OK   $p"
            echo "       $line"
        else
            echo "MISS $p"
        fi
    done

    echo "--- есть ли sdcardfs в ядре ---"
    if grep -qw sdcardfs /proc/filesystems 2>/dev/null; then
        echo "OK   sdcardfs зарегистрирован в /proc/filesystems"
    else
        echo "FAIL sdcardfs НЕ зарегистрирован в /proc/filesystems"
        echo "     ядро собрано без CONFIG_SDCARD_FS — модуль работать не сможет"
    fi

    echo "--- доступен ли splice_read ---"
    if [ -r /proc/kallsyms ]; then
        n=$(grep -c sdcardfs_splice_read /proc/kallsyms 2>/dev/null)
        echo "sdcardfs_splice_read в kallsyms: ${n:-0} совпадений"
    else
        echo "(kallsyms недоступен)"
    fi

    echo "--- конфиг ---"
    if [ -f "$CONF" ]; then
        echo "$CONF:"
        sed 's/^/    /' "$CONF"
    else
        echo "$CONF отсутствует — модуль ничего не делает"
    fi

    echo "===== конец ====="
    echo
} >> "$LOG" 2>&1

# чтобы лог не рос бесконечно
lines=$(wc -l < "$LOG" 2>/dev/null || echo 0)
if [ "$lines" -gt 2000 ]; then
    tail -n 1000 "$LOG" > "$LOG.tmp" && mv -f "$LOG.tmp" "$LOG"
fi
