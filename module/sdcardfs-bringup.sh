#!/system/bin/sh
# Поднимает sdcardfs на /mnt/runtime/*/emulated с ПРАВИЛЬНЫМИ опциями для
# каждой точки.
#
# ПОЧЕМУ НЕ /system/bin/sdcard:
#   AOSP-овский sdcard.cpp создаёт только /mnt/runtime/default/emulated реальным
#   mount(2), а read/write/full — через sdcardfs_setup_bind_remount(), то есть
#   bind + MS_REMOUNT с новыми mask=/gid=. В этом ядре
#   sdcardfs_remount_fs() опции вообще не разбирает (заглушка, которая только
#   проверяет flags), а vfsopts живут в superblock. Итог: все четыре маунта
#   оказываются биндами одного superblock с опциями default — gid=1015, mask=6.
#   Проверено на устройстве: /mnt/runtime/{read,write,full}/emulated получают
#   gid=1015,mask=6 вместо gid=9997,mask=23|7.
#
# РЕШЕНИЕ: каждый маунт создаётся отдельным mount(2). Тогда у каждого свой
# анонимный superblock (mount_nodev) и свои vfsopts — как и задумано в
# комментарии драйвера sdcardfs.h:225-227.
#
# Опции повторяют system/core/sdcard/sdcard.cpp:
#   default → mask=6  (oct 0006), gid=1015 AID_SDCARD_RW
#   read    → mask=23 (oct 0027), gid=9997 AID_EVERYBODY
#   write   → mask=7  (oct 0007), gid=9997   (vold всегда передаёт -w)
#   full    → mask=7  (oct 0007), gid=9997
#
# ВАЖНО: mask печатается в десятичном виде, потому что sdcard.cpp использует
# StringPrintf("mask=%d", mask) над octal-литералом: 0006→"6", 0027→"23", 0007→"7".

SRC=/data/media
COMMON="fsuid=1023,fsgid=1023,multiuser,derive_gid,default_normal,unshared_obb,userid=0"

QUIET=0
[ "$1" = "-q" ] && QUIET=1
log() { [ "$QUIET" = "1" ] || echo "$@"; }

mount_one() {
    d="$1"; mask="$2"; gid="$3"
    p="/mnt/runtime/$d/emulated"

    if grep -q " $p " /proc/mounts 2>/dev/null; then
        log "  уже смонтировано: $p"
        return 0
    fi
    mkdir -p "$p" 2>/dev/null
    if mount -t sdcardfs -o "$COMMON,mask=$mask,gid=$gid" "$SRC" "$p"; then
        log "  OK   $p  (mask=$mask gid=$gid)"
        return 0
    fi
    log "  FAIL $p  (mask=$mask gid=$gid)"
    return 1
}

rc=0
mount_one default 6  1015 || rc=1
mount_one read    23 9997 || rc=1
mount_one write   7  9997 || rc=1
mount_one full    7  9997 || rc=1
exit $rc
