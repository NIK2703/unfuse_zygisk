#!/system/bin/sh
#
# post-fs-data.sh — поднимает sdcardfs на /mnt/runtime/*/emulated.
#
# Зачем это нужно:
#   На этой прошивке external_storage.sdcardfs.enabled=0 (задано в
#   /vendor/build.prop), поэтому vold НЕ выполняет /system/bin/sdcard и
#   /mnt/runtime/{default,read,write,full}/emulated остаются пустыми каталогами
#   на tmpfs. Без них модулю нечего подкладывать под /mnt/user/<u>/emulated.
#
# Почему не полагаемся на vold:
#   Даже если включить свойство, vold запустит /system/bin/sdcard, а тот для
#   read/write/full использует bind + MS_REMOUNT. В этом ядре
#   sdcardfs_remount_fs() опции не разбирает вообще (заглушка), а vfsopts живут
#   в superblock, поэтому все четыре маунта получают опции default
#   (gid=1015, mask=6) и становятся бесполезны для приложений.
#   Подробности — в sdcardfs-bringup.sh.
#
# Свойство external_storage.sdcardfs.enabled НЕ трогаем: vold продолжает
# работать по прежней (FUSE) ветке, /mnt/pass_through остаётся как есть,
# и мы не вмешиваемся в работу MediaProvider.
#

MODDIR=${MODDIR:-${0%/*}}
LOG=/data/adb/sdcardfs_restore.log

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: $*" >> "$LOG"; }

log "старт"

# Ждём, пока vold создаст /data/media/0 (до ~30 секунд).
i=0
while [ ! -d /data/media/0 ] && [ "$i" -lt 60 ]; do
    sleep 0.5
    i=$((i + 1))
done

if [ ! -d /data/media/0 ]; then
    log "ОШИБКА: /data/media/0 так и не появился, bringup пропущен"
    exit 0
fi

# Маркер аварийного отключения — ничего не делаем.
for m in /data/adb/sdcardfs_restore.disable "$MODDIR/disable"; do
    if [ -f "$m" ]; then
        log "найден маркер $m — bringup пропущен"
        exit 0
    fi
done

if [ ! -f "$MODDIR/sdcardfs-bringup.sh" ]; then
    log "ОШИБКА: нет $MODDIR/sdcardfs-bringup.sh"
    exit 0
fi

# Перемаркировка корня /data/media: media_userdir_file -> media_rw_data_file.
# Без неё stat()/ls() самого /storage/emulated отдают EACCES (sdcardfs
# форвардит getattr корня в нижний инод, а у media_userdir_file для
# appdomain/coredomain разрешён только search). Обязательный шаг.
# Делаем ДО старта Zygote, поэтому все приложения видят уже верный ярлык.
if [ -f "$MODDIR/relabel-media.sh" ]; then
    MODDIR="$MODDIR" sh "$MODDIR/relabel-media.sh"
fi

# Права на каталоги, через которые проходит sdcardfs: /mnt/runtime (0700 root)
# и /data/media (0550 uid 1023). Без этого маунт из preAppSpecialize получает
# EACCES без единого AVC. Управляется !relax= в config.
if [ -f "$MODDIR/relax-storage.sh" ]; then
    MODDIR="$MODDIR" sh "$MODDIR/relax-storage.sh"
fi

{
    echo "----- post-fs-data: $(date) -----"
    sh "$MODDIR/sdcardfs-bringup.sh"
} >> "$LOG" 2>&1

log "готово"
