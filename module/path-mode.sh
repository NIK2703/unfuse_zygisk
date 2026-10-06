#!/system/bin/sh
#
# path-mode.sh — показать или задать режим пути модуля sdcardfs-restore.
#
# Режим хранится в /data/adb/sdcardfs_restore.conf (ключ path) и переживает и
# перезагрузку, и переустановку модуля. Этот скрипт — просто удобная обёртка,
# чтобы не править конфиг руками и не забыть, что там уже написано.
#
# Использование:
#
#   sh path-mode.sh              показать текущий режим и откуда он взялся
#   sh path-mode.sh raw          задать режим: только сырое дерево + ACL
#   sh path-mode.sh sdcardfs     задать режим: только основной путь
#   sh path-mode.sh auto         задать режим по умолчанию
#   sh path-mode.sh apply        применить текущий режим прямо сейчас
#
# `apply` гоняет storage.sh без перезагрузки: этого достаточно, чтобы сменить
# сырое дерево на sdcardfs или обратно и перезапустить приложение. Но vold
# готовит CE-хранилище пользователя позже, поэтому полностью настройка
# устаканивается после перезагрузки — при сомнениях перезагрузитесь.
#
# MODDIR подставляется загрузчиком модулей; при ручном запуске берётся из пути
# к самому скрипту.
#

MODDIR="${MODDIR:-${0%/*}}"
CONF=/data/adb/sdcardfs_restore.conf
LOG=/data/adb/sdcardfs_restore.log
LEGACY_RAW=/data/adb/sdcardfs_restore.force_raw

stamp() { date '+%Y-%m-%d %H:%M:%S'; }

# Значение ключа $1 — тот же разбор, что в storage.sh.
conf_get() {
    [ -r "$CONF" ] || return 1
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$CONF" 2>/dev/null \
        | sed 's/[[:space:]]*#.*$//' | sed 's/[[:space:]]*$//' | tail -n 1
}

show() {
    m=$(conf_get path)
    if [ -n "$m" ]; then
        echo "режим: $m (из $CONF)"
    elif [ -e "$LEGACY_RAW" ]; then
        echo "режим: raw (устаревший синоним $LEGACY_RAW, в конфиге path нет)"
    else
        echo "режим: auto (в конфиге нет path — поведение по умолчанию)"
    fi

    if [ -e "$LEGACY_RAW" ]; then
        echo "внимание: на месте $LEGACY_RAW;"
        echo "          он означает raw, но явный path в конфиге главнее."
    fi

    # На чём модуль сошёлся по факту — по эффективному маунту.
    for p in /mnt/runtime/default/emulated /mnt/runtime/read/emulated \
             /mnt/runtime/write/emulated   /mnt/runtime/full/emulated; do
        t=""
        while read -r _dev mp ty _rest; do
            [ "$mp" = "$p" ] && t="$ty"
        done < /proc/mounts
        printf '  %-32s %s\n' "$p" "${t:-—}"
    done
}

set_mode() {
    case "$1" in
        auto|sdcardfs|raw) ;;
        *)
            echo "неизвестный режим: $1 (нужен auto, sdcardfs или raw)" >&2
            exit 2
            ;;
    esac

    # Пишем конфиг целиком, а не правим строку: так в нём не остаётся старых
    # значений, если строк было несколько.
    tmp="$CONF.new"
    {
        echo "# sdcardfs_restore.conf — создан path-mode.sh $(stamp)"
        echo "# Значения path: auto (по умолчанию), sdcardfs (строго основной"
        echo "# путь), raw (строго сырое дерево + ACL). Подробности — README §6.1."
        echo "path=$1"
    } > "$tmp" 2>/dev/null

    if [ ! -s "$tmp" ]; then
        echo "не удалось записать $tmp" >&2
        exit 3
    fi
    mv "$tmp" "$CONF" 2>/dev/null || { echo "не удалось положить $CONF" >&2; exit 3; }
    chmod 0644 "$CONF" 2>/dev/null
    echo "[$(stamp)] path-mode: режим $1 записан в $CONF" >> "$LOG"
    echo "режим: $1 (записан в $CONF)"
    echo "применится после перезагрузки; применить сейчас — sh path-mode.sh apply"
}

apply() {
    if [ ! -x "$MODDIR/storage.sh" ]; then
        echo "нет $MODDIR/storage.sh — запускайте скрипт из каталога модуля" >&2
        exit 1
    fi
    sh "$MODDIR/storage.sh" path-mode
    echo "storage.sh отработал; журнал: $LOG"
}

case "${1:-}" in
    "")        show ;;
    apply)     apply ;;
    *)         set_mode "$1" ;;
esac
