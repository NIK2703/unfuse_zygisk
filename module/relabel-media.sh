#!/system/bin/sh
#
# relabel-media.sh — снимает ярлык media_userdir_file с КОРНЯ внутреннего
# хранилища (/data/media) и с корней adoptable-хранилищ (/mnt/expand/<uuid>/media).
#
# ---------------------------------------------------------------------------
# Зачем это нужно
# ---------------------------------------------------------------------------
# sdcardfs — это «обёртка»: собственных данных он не хранит, а пробрасывает
# операции к НИЖНЕМУ слою и делает это от имени вызывающего процесса. Поэтому
# при обращении приложения к /storage/emulated проверки SELinux идут не только
# по метке самого sdcardfs, но и по метке нижнего файла в /data/media.
#
# У КОРНЯ нижнего слоя ярлык особый:
#
#     private/file_contexts:633   /data/media     u:object_r:media_userdir_file:s0
#     private/file_contexts:634   /data/media/.*  u:object_r:media_rw_data_file:s0
#
# и для media_userdir_file разрешено РОВНО одно право:
#
#     private/domain.te:252
#     allow { coredomain appdomain } media_userdir_file:dir search;
#
# search пускает внутрь каталога, но не даёт getattr. Значит stat()/ls() самого
# корня отваливаются:
#
#     stat /mnt/runtime/full/emulated    -> Permission denied   (без единого AVC!)
#     stat /mnt/runtime/full/emulated/0  -> ok
#
# AVC нет потому, что доступ appdomain к /data/media помечен dontaudit
# (см. private/zygote.te:233 — комментарий прямо говорит, что sdcardfs
# читает/открывает каталоги за приложение).
#
# А на media_rw_data_file у приложений права полные:
#
#     private/app.te:149
#     allow appdomain media_rw_data_file:dir create_dir_perms;
#
# ---------------------------------------------------------------------------
# Что делаем
# ---------------------------------------------------------------------------
# Перемаркируем корень в media_rw_data_file. Это ровно та схема, что была на
# Android 10 и раньше: тогда /data/media описывался одним правилом
#     /data/media(/.*)?  u:object_r:media_rw_data_file:s0
# а отдельный тип media_userdir_file появился позже именно чтобы закрыть корень.
#
# Права доступа (DAC) при этом НЕ меняются: /data/media остаётся 0551 uid 1023,
# а /data/media/0 — 2770 uid/gid 1023. Перемаркировка влияет только на
# SELinux-проверку getattr для корня sdcardfs, поэтому посторонний процесс
# по-прежнему не может ни листать /data/media напрямую, ни писать туда.
#
# Патчить sepolicy для этого не требуется: право на media_rw_data_file у
# appdomain уже есть. Именно поэтому здесь chcon, а не sepolicy.rule.
#
# Скрипт идемпотентен: повторный запуск ничего не меняет.
# Отключается строкой !relabel=0 в config.
#
# Применять ДО старта Zygote (post-fs-data) и повторно после vold (service),
# потому что vold может вернуть ярлык обратно, когда готовит /data/media.

MODDIR=${MODDIR:-${0%/*}}
CONF="$MODDIR/config"
LOG=/data/adb/sdcardfs_restore.log

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] relabel: $*" >> "$LOG"; }

mode=1
if [ -f "$CONF" ]; then
    v=$(sed -n 's/^!relabel=//p' "$CONF" | tail -1)
    [ -n "$v" ] && mode="$v"
fi

case "$mode" in
    0|off|none|no)
        log "выключено (!relabel=$mode)"
        exit 0
        ;;
esac

relabel_one() {
    p="$1"
    [ -e "$p" ] || return 0

    cur=$(ls -Zd "$p" 2>/dev/null | awk '{print $1}')
    case "$cur" in
        *:media_rw_data_file:*)
            return 0
            ;;
    esac

    if chcon u:object_r:media_rw_data_file:s0 "$p" 2>>"$LOG"; then
        log "$p: $cur -> media_rw_data_file"
    else
        log "$p: НЕ УДАЛОСЬ перемаркировать (осталось $cur)"
    fi
}

relabel_one /data/media

# adoptable storage: /mnt/expand/<uuid>/media имеет тот же ярлык
for d in /mnt/expand/*/media; do
    [ -e "$d" ] && relabel_one "$d"
done

exit 0
