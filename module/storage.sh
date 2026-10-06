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
# Какой путь задействован, решает шаг 0 — он читает /data/adb/sdcardfs_restore.conf
# (ключ path, см. README §6.1). По умолчанию auto: приоритет у sdcardfs —
# поднять его и проверить по факту, а если ядро его не умеет или проверка не
# прошла, уйти на сырое дерево. Проверка нужна потому, что код возврата
# mount(2) ничего не говорит о том, применило ли ядро опции маунта (шаг 2a).
#

STAGE="${1:-storage}"
MODDIR="${MODDIR:-${0%/*}}"
LOG=/data/adb/sdcardfs_restore.log

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $STAGE: $*" >> "$LOG"; }

# --- 0. Какой путь задействовать: конфиг модуля -------------------------------
#
# Выбор пути — настройка модуля, а не состояние текущей загрузки: она лежит в
# /data/adb/sdcardfs_restore.conf и переживает и перезагрузку, и переустановку
# модуля (customize.sh копирует туда конфиг из архива только один раз, если
# файла ещё нет). Ключ один — path, значения auto|sdcardfs|raw, смысл каждого
# расписан в самом конфиге и в README §6.1.
#
# Почему /data/adb, а не каталог модуля: /data/adb/modules/sdcardfs_restore/
# целиком перезаписывается при каждой установке, поэтому файл-настройка внутри
# него молча пропадал бы при обновлении. /data/adb/sdcardfs_restore.conf лежит
# рядом с журналом и не трогается установщиком.
#
# Файл разбирается здесь, а не подключается через `.`: он доступен на запись
# любому процессу с root, а модуль не должен исполнять код из /data/adb.
#
# Неизвестное или пустое значение = auto. Модуль не имеет права остаться без
# памяти из-за опечатки в конфиге, поэтому всё непонятное трактуется как
# поведение по умолчанию.
#
CONF=/data/adb/sdcardfs_restore.conf

# Значение ключа $1 из конфига. Печатает последнее вхождение: поздняя строка
# перекрывает раннюю, как в обычном конфиге.
conf_get() {
    [ -r "$CONF" ] || return 1
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$CONF" 2>/dev/null \
        | sed 's/[[:space:]]*#.*$//' | sed 's/[[:space:]]*$//' | tail -n 1
}

PATH_MODE=$(conf_get path)
PATH_SRC="конфиг $CONF"

if [ -z "$PATH_MODE" ]; then
    PATH_MODE=auto
    PATH_SRC="по умолчанию (в конфиге нет path)"
fi

case "$PATH_MODE" in
    auto|sdcardfs|raw) ;;
    *)
        bad="$PATH_MODE"
        log "конфиг: неизвестное path=$bad — беру auto"
        PATH_MODE=auto
        PATH_SRC="по умолчанию (path=$bad не распознан)"
        ;;
esac

# Устаревший синоним path=raw: файл-метка, которой альтернативный путь
# включался до появления конфига. Оставлен, чтобы прежние сборки и чужие
# инструкции продолжали работать. Явный path в конфиге главнее: если файл есть,
# а конфиг просит не raw, это попадает в журнал, а не молча меняет поведение.
LEGACY_RAW=/data/adb/sdcardfs_restore.force_raw
if [ -e "$LEGACY_RAW" ]; then
    if [ "$PATH_SRC" = "конфиг $CONF" ]; then
        log "конфиг: $LEGACY_RAW на месте, но конфиг задаёт path=$PATH_MODE — конфиг главнее"
    else
        PATH_MODE=raw
        PATH_SRC="устаревший $LEGACY_RAW"
    fi
fi

log "режим пути: $PATH_MODE ($PATH_SRC)"

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

# Точки основного пути и то, что модуль у каждой просит: <имя>:<mask>:<gid>.
#
# Список — единственный источник правды: по нему и монтируем, и проверяем,
# поэтому проверка не может разойтись с тем, что просили у mount(2).
#
# mask печатается в десятичном виде: в sdcard.cpp это StringPrintf("mask=%d",
# mask) над octal-литералом, поэтому 0006 -> 6, 0027 -> 23, 0007 -> 7.
# full: mask=0007 и gid=9997 (AID_EVERYBODY) дают 0770 на каталоги и 0660 на
# файлы для любого процесса.
#
SDCARDFS_POINTS="default:6:1015 read:23:9997 write:7:9997 full:7:9997"

# Опции маунта точки так, как их показывает ядро: "<тип> <опции>". Пусто, если
# точки в /proc/mounts нет.
#
# Берётся последняя запись, а не первая: маунты идут в порядке наложения,
# поэтому последняя — верхняя (та же причина, что и в fs_type()).
mount_line() {
    r=""
    while read -r _dev mp ty opts _rest; do
        [ "$mp" = "$1" ] && r="$ty $opts"
    done < /proc/mounts
    [ -n "$r" ] && echo "$r"
}

# Есть ли опция $2 среди опций $1. Список оборачивается запятыми, чтобы
# "gid=1015" не совпало с "fsgid=1015": шаблон ",gid=1015," требует запятую
# непосредственно перед именем, а в "fsgid=1015" там стоит "s".
has_opt() {
    case ",$1," in
        *",$2,"*) return 0 ;;
        *)        return 1 ;;
    esac
}

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

mount_all() {
    for spec in $SDCARDFS_POINTS; do
        rest="${spec#*:}"
        mount_one "${spec%%:*}" "${rest%%:*}" "${rest##*:}"
    done
}

umount_all() {
    for spec in $SDCARDFS_POINTS; do
        p="/mnt/runtime/${spec%%:*}/emulated"
        [ "$(fs_type "$p")" = "$SDCARDFS_FS" ] && umount "$p" 2>/dev/null
    done
}

# --- 2a. Проверка основного пути по факту -------------------------------------
#
# mount(2) возвращает 0 и тогда, когда ядро приняло маунт, но не применило
# vfsopts. На этой прошивке это не теория: sdcardfs_remount_fs() — заглушка, и
# если бы четыре точки делили один суперблок, опции у всех были бы от первой
# (gid=1015, mask=6). Приложения получили бы не тот доступ, и никакой ошибки
# нигде бы не было — ни в журнале, ни в коде возврата.
#
# Поэтому основной путь принимается не по коду возврата mount(2), а по факту:
#
#   1. под точкой лежит именно sdcardfs;
#   2. в опциях маунта стоят те mask и gid, что модуль просил.
#
# Проверка суперблоков отдельно не нужна: /proc/mounts показывает опции
# суперблока, поэтому если бы точки его делили, вторая проверка уже увидела бы
# у одной из них чужую маску.
#
# Вторая проверка срабатывает, только если ядро эти опции показывает вообще.
# Нет в строке маунта ни mask=, ни gid= — значит эта сборка sdcardfs их не
# печатает, судить не по чему, и точка не считается провалившейся. Так проверка
# не отвергнет рабочий основной путь на чужом ядре, где show_options устроен
# иначе (проверено на устройстве: здесь ядро печатает и mask=, и gid=).
#
sdcardfs_verify() {
    for spec in $SDCARDFS_POINTS; do
        name="${spec%%:*}"; rest="${spec#*:}"
        want_mask="${rest%%:*}"; want_gid="${rest##*:}"
        p="/mnt/runtime/$name/emulated"

        line=$(mount_line "$p")
        if [ -z "$line" ]; then
            log "проверка: $p не смонтирована"
            return 1
        fi
        ty="${line%% *}"; opts="${line#* }"

        if [ "$ty" != "$SDCARDFS_FS" ]; then
            log "проверка: $p под $ty, а не $SDCARDFS_FS"
            return 1
        fi

        case ",$opts," in
            *,mask=*)
                if ! has_opt "$opts" "mask=$want_mask"; then
                    log "проверка: $p с чужой маской — ждали mask=$want_mask," \
                        "ядро отдало: $opts"
                    return 1
                fi
                ;;
        esac

        case ",$opts," in
            *,gid=*)
                if ! has_opt "$opts" "gid=$want_gid"; then
                    log "проверка: $p с чужим gid — ждали gid=$want_gid," \
                        "ядро отдало: $opts"
                    return 1
                fi
                ;;
        esac
    done
    return 0
}

# --- 2b. Что делаем на шаге 2 -------------------------------------------------
#
# Решает PATH_MODE:
#   raw      — sdcardfs не поднимаем вообще, а прежние маунты снимаем тут же,
#              чтобы модуль ушёл на сырое дерево, не дожидаясь перезагрузки;
#   auto     — приоритет у sdcardfs: поднимаем и ПРОВЕРЯЕМ; не поднялся или не
#              прошёл проверку — снимаем и уходим на сырое дерево;
#   sdcardfs — то же, но без права уйти на сырое: при любой неудаче модуль
#              пишет в журнал, что именно не сошлось, и выходит с кодом 1.
#
if [ "$PATH_MODE" = raw ]; then
    log "path=raw — основной путь выключен настройкой модуля"
    umount_all
elif grep -qw sdcardfs /proc/filesystems 2>/dev/null; then
    mount_all

    if sdcardfs_verify; then
        log "основной путь готов и проверен: sdcardfs на /mnt/runtime/*/emulated"
        exit 0
    fi

    if [ "$PATH_MODE" = sdcardfs ]; then
        # Строгий режим: подмены нет, но и снимать маунт не будем — он просили
        # именно его, а в журнале уже лежит строка о том, что не сошлось.
        log "path=sdcardfs: основной путь не прошёл проверку — оставляю как есть," \
            "альтернативный запрещён настройкой"
        exit 1
    fi

    log "основной путь не прошёл проверку — снимаю его и перехожу на альтернативный"
    umount_all
else
    if [ "$PATH_MODE" = sdcardfs ]; then
        log "path=sdcardfs, но в ядре нет sdcardfs (/proc/filesystems) —" \
            "приложения останутся без памяти: альтернативный путь запрещён настройкой"
        exit 1
    fi
    log "в ядре нет sdcardfs (/proc/filesystems) — основной путь недоступен"
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
# Одних проходов, однако, не хватает: vold дописывает default-ACL у
# /data/media/<user> и у каталогов пакетов уже ПОСЛЕ стадии service — при
# подготовке CE-хранилища пользователя, когда установлен его ключ. Гонку
# закрывает не повтор и не сторож, а патч vold: tools/vold-noacl.c делает
# vold::SetDefaultAcl() пустышкой. Ставится он в post-fs-data.sh и service.sh.
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

# --- 4. Отчёт о состоянии ACL -------------------------------------------------
#
# Не сторож и не лечение — просто строка в журнал: на чём сошлись после прохода.
# Нужна, чтобы по одному /data/adb/sdcardfs_restore.log было видно, дошло ли
# дело до конца, не лазая в ACL руками. Практического влияния не имеет.
#
if [ -d /data/media/0 ]; then
    if "$FIX" --check /data/media/0 >>"$LOG" 2>&1; then
        log "ACL /data/media/0: запись 9997 на месте в обеих ACL"
    else
        log "ACL /data/media/0: записи 9997 нет в обеих ACL — см. --check в журнале"
    fi
fi

exit 0
