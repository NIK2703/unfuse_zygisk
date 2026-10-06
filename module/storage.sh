#!/system/bin/sh
#
# storage.sh — готовит внутреннее хранилище к работе модуля. Вызывается дважды:
# из post-fs-data.sh (до старта Zygote) и из service.sh (после vold). Оба шага
# идемпотентны.
#

STAGE="${1:-storage}"
LOG=/data/adb/sdcardfs_restore.log

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $STAGE: $*" >> "$LOG"; }

# --- 1. Ярлык корня /data/media: media_userdir_file -> media_rw_data_file -----
#
# sdcardfs не заводит отдельного инода для корня точки монтирования: getattr()
# корня форвардится в нижний инод (/data/media). Для media_userdir_file у
# appdomain и coredomain разрешён ровно один search, без getattr (domain.te:252),
# поэтому без перемаркировки stat()/ls() самого /storage/emulated отдают EACCES —
# причём без единого AVC, доступ помечен dontaudit. Содержимое работает и так:
# там ярлык media_rw_data_file, на который у appdomain полные права (app.te:149).
#
# media_rw_data_file — тот же ярлык, что был у /data/media на Android 10 и раньше.
# Права (DAC) не меняются, поэтому листать /data/media напрямую по-прежнему
# нельзя. Патчить sepolicy не нужно: право уже есть, а relabelto для домена
# zygote запрещён neverallow (domain.te:791) — поэтому перемаркировка делается
# здесь, а не из процесса приложения.
#
cur=$(ls -Zd /data/media 2>/dev/null | awk '{print $1}')
case "$cur" in
    *:media_rw_data_file:*) ;;
    *)
        if chcon u:object_r:media_rw_data_file:s0 /data/media 2>>"$LOG"; then
            log "/data/media: $cur -> media_rw_data_file"
        else
            log "/data/media: НЕ УДАЛОСЬ перемаркировать (осталось $cur)"
        fi
        ;;
esac

# --- 2. sdcardfs на /mnt/runtime/*/emulated — источник для модуля -------------
#
# На этой прошивке external_storage.sdcardfs.enabled=0 (задано в
# /vendor/build.prop), поэтому vold НЕ выполняет /system/bin/sdcard и
# /mnt/runtime/*/emulated остаются пустыми каталогами: модулю нечего подкладывать
# под /mnt/user/<user>/emulated.
#
# Почему не /system/bin/sdcard: AOSP-овский sdcard.cpp создаёт реальным mount(2)
# только default/emulated, а read/write/full — через bind + MS_REMOUNT. В этом
# ядре sdcardfs_remount_fs() опции не разбирает (заглушка), а vfsopts живут в
# superblock, поэтому все четыре маунта получили бы опции default (gid=1015,
# mask=6). Каждый маунт создаётся отдельным mount(2) — тогда у каждого свой
# анонимный superblock и свои vfsopts.
#
# mask печатается в десятичном виде: в sdcard.cpp это StringPrintf("mask=%d",
# mask) над octal-литералом, поэтому 0006 -> 6, 0027 -> 23, 0007 -> 7.
# Модуль берёт full: mask=0007 и gid=9997 (AID_EVERYBODY) дают 0770 на каталоги и
# 0660 на файлы для любого процесса.
#
COMMON="fsuid=1023,fsgid=1023,multiuser,derive_gid,default_normal,unshared_obb,userid=0"

mount_one() {
    p="/mnt/runtime/$1/emulated"

    # Уже наш маунт — ничего не делаем.
    if grep -q " $p sdcardfs " /proc/mounts 2>/dev/null; then
        return 0
    fi
    # Точку занял кто-то другой — не трогаем, иначе модуль молча получит под
    # /mnt/user/<user>/emulated не sdcardfs.
    if grep -q " $p " /proc/mounts 2>/dev/null; then
        log "ВНИМАНИЕ: $p занят другим маунтом, не трогаю"
        return 1
    fi

    mkdir -p "$p" 2>/dev/null
    if mount -t sdcardfs -o "$COMMON,mask=$2,gid=$3" /data/media "$p"; then
        log "OK   $p (mask=$2 gid=$3)"
    else
        log "FAIL $p (mask=$2 gid=$3)"
    fi
}

mount_one default 6  1015
mount_one read    23 9997
mount_one write   7  9997
mount_one full    7  9997

exit 0
