#!/system/bin/sh
#
# storage.sh — готовит внутреннее хранилище к работе модуля. Вызывается дважды:
# из post-fs-data.sh (до старта Zygote) и из service.sh (после vold). Оба шага
# идемпотентны.
#
# У модуля два пути, и этот скрипт обслуживает оба:
#
#   основной      — поднять sdcardfs на /mnt/runtime/*/emulated (шаг 2);
#   альтернативный — если sdcardfs в ядре нет, расставить ACL на сыром дереве
#                    /data/media (шаг 3), чтобы приложения могли в него войти.
#
# Какой путь задействован, решает шаг 2: если под всеми четырьмя точками
# /mnt/runtime/*/emulated действительно sdcardfs, альтернативный не нужен.
# Проверка — по эффективному маунту, см. fs_type() ниже.
#

STAGE="${1:-storage}"
MODDIR="${MODDIR:-${0%/*}}"
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
# Патчить sepolicy не нужно: право уже есть, а relabelto для домена zygote
# запрещён neverallow (domain.te:791) — поэтому перемаркировка делается здесь, а
# не из процесса приложения.
#
# На альтернативном пути это ещё и обязательно: под /storage/emulated ложится
# само дерево /data/media, и его корень должен быть media_rw_data_file, иначе
# appdomain не получит даже search.
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

# --- 2. Основной путь: sdcardfs на /mnt/runtime/*/emulated --------------------
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
SDCARDFS_FS=sdcardfs

# Тип файловой системы, эффективно лежащей под точкой.
#
# Берётся последняя запись /proc/mounts для этого пути: маунты идут в порядке
# наложения, поэтому последняя — верхняя. Простой grep по /proc/mounts тут
# ошибается: он видит и то, что лежит ПОД точкой, и решает, что sdcardfs на
# месте, даже если поверх положили что-то другое.
#
# Только средствами шелла, без stat(1): на стадиях модуля KernelSU подставляет
# свой busybox, а busybox-овский `stat -c %T` печатает UNKNOWN вместо
# магического числа — проверка молча ломается (проверено на устройстве:
# toybox даёт 0x5dca2df5, busybox даёт UNKNOWN). Чтение /proc/mounts зависит
# только от read, который есть в любой сборке.
fs_type() {
    t=""
    while read -r _dev mp ty _rest; do
        [ "$mp" = "$1" ] && t="$ty"
    done < /proc/mounts
    echo "$t"
}

COMMON="fsuid=1023,fsgid=1023,multiuser,derive_gid,default_normal,unshared_obb,userid=0"

mount_one() {
    p="/mnt/runtime/$1/emulated"

    # Уже наш маунт — не трогаем. Нужно на стадии service: post-fs-data мог
    # уже всё поднять.
    if [ "$(fs_type "$p")" = "$SDCARDFS_FS" ]; then
        return 0
    fi

    mkdir -p "$p" 2>/dev/null
    if mount -t sdcardfs -o "$COMMON,mask=$2,gid=$3" /data/media "$p"; then
        log "OK   $p (mask=$2 gid=$3)"
    else
        log "FAIL $p (mask=$2 gid=$3)"
    fi
}

sdcardfs_ready() {
    for m in default read write full; do
        [ "$(fs_type "/mnt/runtime/$m/emulated")" = "$SDCARDFS_FS" ] || return 1
    done
    return 0
}

if grep -qw sdcardfs /proc/filesystems 2>/dev/null; then
    mount_one default 6  1015
    mount_one read    23 9997
    mount_one write   7  9997
    mount_one full    7  9997
else
    log "в ядре нет sdcardfs (/proc/filesystems) — основной путь недоступен"
fi

if sdcardfs_ready; then
    log "основной путь готов: sdcardfs на /mnt/runtime/*/emulated"
    exit 0
fi

log "основной путь не поднялся — перехожу на альтернативный"

# --- 3. Альтернативный путь: ACL на сыром дереве ------------------------------
#
# Под /mnt/user/<user>/emulated модуль положит само /data/media (как AOSP на
# /mnt/pass_through, vold-16/Utils.cpp:1691). Но дерево принадлежит 1023:1023 с
# режимами 0550/2770/0670, а в группах приложений 1023 нет — без правки они не
# войдут даже в корень. sdcardfs эту работу делает сам, отдавая каталоги 0770 и
# файлы 0660 для gid 9997; здесь то же самое выставляется ACL.
#
# 9997 (AID_EVERYBODY) — общая группа всех приложений профиля
# (android_filesystem_config.h:166) и ровно тот gid, что у sdcardfs-маунтов
# read/write/full. Поэтому доступ получают все приложения, а не список избранных.
#
# Именованная запись в ACL, а не chmod, потому что:
#   - vold при каждой загрузке сбрасывает владельца и режим /data/media,
#     Android, Android/data, Android/obb и Android/media (fs_prepare_dir ->
#     chown+chmod). chmod правит в ACL только USER_OBJ/GROUP_OBJ/MASK/OTHER и не
#     трогает именованные записи — выданный доступ это переживает;
#   - umask приложений 0077 обнулял бы групповые биты у всего, что они создают.
#     Но если у каталога есть default ACL, ядро вообще не применяет umask:
#     vfs_create() пропускает `mode &= ~current_umask()`, а posix_acl_create()
#     считает режим пересечением с ACL. Файл выходит 0660 с унаследованной
#     записью для 9997 — и его сразу видят все приложения.
#
# Шаг 3 идемпотентен и повторяется на обеих стадиях: vold пересобирает свои
# каталоги после post-fs-data, поэтому проход на стадии service возвращает ACL на
# место (и заодно накрывает пакеты, чьи каталоги vold создал за это время).
#
FIX="$MODDIR/tools/storage-fix"
if [ ! -x "$FIX" ]; then
    log "ВНИМАНИЕ: нет $FIX — приложения останутся без памяти"
    exit 1
fi

# Корню тома — только r-x: его нужно пройти, но писать в него нечего.
"$FIX" --traverse /data/media >>"$LOG" 2>&1

if [ -d /data/media/0 ]; then
    "$FIX" /data/media/0 >>"$LOG" 2>&1
else
    log "нет /data/media/0 — раздел ещё не развёрнут, повтор на стадии service"
fi

# Устаревший OBB вне каталога пользователя (unshared_obb).
if [ -d /data/media/obb ]; then
    "$FIX" /data/media/obb >>"$LOG" 2>&1
fi

log "альтернативный путь: ACL с группой 9997 расставлены"

exit 0
