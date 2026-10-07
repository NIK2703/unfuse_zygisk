#!/system/bin/sh
#
# path-mode.sh — show or set the module's path mode.
#
# The mode lives in unfuse_zygisk.conf in the module directory (key path). This
# script is a convenience wrapper so the config does not have to be edited by
# hand; customize.sh carries the setting over when the module is reinstalled.
#
# Usage:
#
#   sh path-mode.sh              show the current mode and where it came from
#   sh path-mode.sh fuse         raw tree with FUSE cut off in vold
#   sh path-mode.sh acl          raw tree + ACL only
#   sh path-mode.sh sdcardfs     main path only
#   sh path-mode.sh auto         default behaviour
#   sh path-mode.sh apply        apply the current mode right now
#
# acl was formerly called raw; the old name is accepted and means the same, so
# old configs and instructions keep working.
#
# fuse is the deepest of the modes: post-fs-data.sh points tools/vold-fusefs at
# vold, which redirects vold's mount() stub so MountUserFuse() binds
# /data/media onto the target instead of mounting FUSE at all. The raw tree then
# reaches apps with no FUSE daemon in between. It still needs the ACL pass, so
# storage.sh runs as usual; what it does not need is sdcardfs.
#
# `apply` runs storage.sh without a reboot: enough to switch between the raw
# tree and sdcardfs and restart an app. vold prepares the user's CE storage
# later, though, so the setting only fully settles after a reboot — when in
# doubt, reboot.
#
# MODDIR is set by the module loader; when run by hand it comes from the
# script's own path.
#

MODDIR="${MODDIR:-${0%/*}}"
CONF="$MODDIR/unfuse_zygisk.conf"
LOG=/data/adb/unfuse_zygisk.log
LEGACY_RAW=/data/adb/unfuse_zygisk.force_raw

stamp() { date '+%Y-%m-%d %H:%M:%S'; }

# Value of key $1 — same parsing as in storage.sh.
conf_get() {
    [ -r "$CONF" ] || return 1
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$CONF" 2>/dev/null \
        | sed 's/[[:space:]]*#.*$//' | sed 's/[[:space:]]*$//' | tail -n 1
}

show() {
    m=$(conf_get path)
    case "$m" in
        acl|raw) m=acl ;;
    esac
    if [ -n "$m" ]; then
        echo "режим: $m (из $CONF)"
    elif [ -e "$LEGACY_RAW" ]; then
        echo "режим: acl (устаревший синоним $LEGACY_RAW, в конфиге path нет)"
    else
        echo "режим: auto (в конфиге нет path — поведение по умолчанию)"
    fi

    if [ -e "$LEGACY_RAW" ]; then
        echo "внимание: на месте $LEGACY_RAW;"
        echo "          он означает acl, но явный path в конфиге главнее."
    fi

    # What the module actually settled on: the effective mount.
    for p in /mnt/runtime/default/emulated /mnt/runtime/read/emulated \
             /mnt/runtime/write/emulated   /mnt/runtime/full/emulated; do
        t=""
        while read -r _dev mp ty _rest; do
            [ "$mp" = "$p" ] && t="$ty"
        done < /proc/mounts
        printf '  %-32s %s\n' "$p" "${t:-—}"
    done

    # In fuse mode the thing worth reporting is not the mount type but whether
    # vold's trampoline is redirected: the mode is only real if the patch is in
    # vold's memory, and that does not survive a reboot as a file does.
    if [ "$m" = fuse ]; then
        vf="$MODDIR/tools/vold-fusefs"
        if [ ! -x "$vf" ]; then
            echo "  vold-fusefs: нет $vf — FUSE не отключён"
        elif "$vf" --check >/dev/null 2>&1; then
            echo "  vold-fusefs: патч на месте — vold не монтирует FUSE"
        else
            rc=$?
            case "$rc" in
                1) echo "  vold-fusefs: vold не найден — патч не применён" ;;
                2) echo "  vold-fusefs: анкер не разрешился — патч не применён" ;;
                *) echo "  vold-fusefs: трамплин цел — патч НЕ применён (rc=$rc)" ;;
            esac
        fi
    fi
}

set_mode() {
    m="$1"
    case "$m" in
        auto|sdcardfs|acl|fuse) ;;
        raw)                    m=acl ;;   # former name of the same mode
        *)
            echo "неизвестный режим: $1 (нужен auto, sdcardfs, acl или fuse)" >&2
            exit 2
            ;;
    esac

    # The config is rewritten whole rather than patched line by line, so no
    # stale values survive if the key appeared more than once.
    tmp="$CONF.new"
    {
        echo "# unfuse_zygisk.conf — written by path-mode.sh $(stamp)"
        echo "# path: auto (default), sdcardfs (main path only), acl (raw tree + ACL),"
        echo "#       fuse (raw tree, vold's FUSE mount cut off)."
        echo "path=$m"
    } > "$tmp" 2>/dev/null

    if [ ! -s "$tmp" ]; then
        echo "не удалось записать $tmp" >&2
        exit 3
    fi
    mv "$tmp" "$CONF" 2>/dev/null || { echo "не удалось положить $CONF" >&2; exit 3; }
    chmod 0644 "$CONF" 2>/dev/null
    echo "[$(stamp)] path-mode: режим $m записан в $CONF" >> "$LOG"
    echo "режим: $m (записан в $CONF)"
    echo "применится после перезагрузки; применить сейчас — sh path-mode.sh apply"
}

apply() {
    if [ ! -f "$MODDIR/storage.sh" ]; then
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
