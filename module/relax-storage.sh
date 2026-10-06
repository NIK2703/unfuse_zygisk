#!/system/bin/sh
#
# relax-storage.sh — снимает лишние биты доступа с каталогов, через которые
# проходит sdcardfs.
#
# Зачем: модуль монтирует sdcardfs в момент preAppSpecialize. В этот момент
# процесс ещё не root в полном смысле (у него нет CAP_DAC_OVERRIDE), поэтому
#   /mnt/runtime  — создан init с режимом 0700 root:root;
#   /data/media   — режим 0550 uid 1023 (media_rw), нижний слой sdcardfs;
# оказываются недостижимы: stat() и bind источника отдают EACCES, причём БЕЗ
# единого AVC — это чисто файловый отказ, а не SELinux.
#
# Что делаем:
#   /mnt/runtime  0700 -> 0711  проход к уже известному пути (листинг закрыт)
#   /data/media   0550 -> 0551  проход к нижнему слою (/data/media/0 остаётся
#                               2770 uid/gid 1023 и посторонним недостижим)
#
# Директива !relax= в config:
#   !relax=1        (по умолчанию) оба каталога
#   !relax=runtime  только /mnt/runtime — хватает для bind-пути
#   !relax=media    только /data/media  — хватает для прямого маунта sdcardfs
#   !relax=0        ничего не трогать
#
# Скрипт идемпотентен: повторный запуск ничего не меняет.
#

MODDIR=${MODDIR:-${0%/*}}
CONF="$MODDIR/config"
LOG=/data/adb/sdcardfs_restore.log

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] relax: $*" >> "$LOG"; }

mode=1
if [ -f "$CONF" ]; then
    v=$(sed -n 's/^!relax=//p' "$CONF" | tail -1)
    [ -n "$v" ] && mode="$v"
fi

case "$mode" in
    0|off|none|no)
        log "выключено (!relax=$mode)"
        exit 0
        ;;
esac

relax_one() {
    dir="$1"
    want="$2"
    [ -d "$dir" ] || return 0

    cur=$(stat -c '%a' "$dir" 2>/dev/null)
    if [ "$cur" = "$want" ]; then
        return 0
    fi
    if chmod "$want" "$dir" 2>>"$LOG"; then
        log "$dir: $cur -> $want"
    else
        log "$dir: не удалось сменить $cur на $want"
    fi
}

case "$mode" in
    1|all|yes)
        relax_one /mnt/runtime 0711
        relax_one /data/media  0551
        ;;
    runtime)
        relax_one /mnt/runtime 0711
        ;;
    media)
        relax_one /data/media 0551
        ;;
    *)
        log "неизвестное значение !relax=$mode — ничего не делаю"
        ;;
esac

exit 0
