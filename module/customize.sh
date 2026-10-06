# shellcheck shell=sh
#
# customize.sh — called by the Magisk/KernelSU installer while unpacking the
# module: checks the build is present, keeps the tool binary for this
# architecture and sets permissions.
#

SKIPUNZIP=0

case "$ARCH" in
    arm64) ABI=arm64-v8a;   FIX=storage-fix-arm64; NOACL=vold-noacl-arm64 ;;
    arm)   ABI=armeabi-v7a; FIX=storage-fix-arm;   NOACL=vold-noacl-arm   ;;
    *)     abort "! Неподдерживаемая архитектура: $ARCH (нужны arm64-v8a или armeabi-v7a)" ;;
esac

if [ ! -f "$MODPATH/zygisk/$ABI.so" ]; then
    abort "! В модуле нет zygisk/$ABI.so — похоже, сборка не выполнялась (build.sh)"
fi

# The archive carries both tools for every ABI: keep only ours and rename them —
# storage.sh, post-fs-data.sh and service.sh look for these names.
if [ ! -f "$MODPATH/tools/$FIX" ]; then
    abort "! В модуле нет tools/$FIX — похоже, сборка не выполнялась (build.sh)"
fi
if [ ! -f "$MODPATH/tools/$NOACL" ]; then
    abort "! В модуле нет tools/$NOACL — похоже, сборка не выполнялась (build.sh)"
fi
mv "$MODPATH/tools/$FIX"   "$MODPATH/tools/storage-fix"
mv "$MODPATH/tools/$NOACL" "$MODPATH/tools/vold-noacl"
rm -f "$MODPATH/tools/storage-fix-arm64" "$MODPATH/tools/storage-fix-arm" \
      "$MODPATH/tools/vold-noacl-arm64"  "$MODPATH/tools/vold-noacl-arm"

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/zygisk/$ABI.so"     0 0 0644
set_perm "$MODPATH/tools/storage-fix"  0 0 0755
set_perm "$MODPATH/tools/vold-noacl"   0 0 0755
set_perm "$MODPATH/customize.sh"       0 0 0755 2>/dev/null
set_perm "$MODPATH/post-fs-data.sh"    0 0 0755 2>/dev/null
set_perm "$MODPATH/service.sh"         0 0 0755 2>/dev/null
set_perm "$MODPATH/storage.sh"         0 0 0755 2>/dev/null
set_perm "$MODPATH/path-mode.sh"       0 0 0755 2>/dev/null
set_perm "$MODPATH/status.sh"          0 0 0755 2>/dev/null

# webroot is deliberately left alone beyond the set_perm_recursive above: the
# loader sets its permissions and SELinux context at install time, and the
# KernelSU docs advise against touching it by hand. Our set_perm calls change
# owner and mode only, never the context.

# Module id — so the paths below and the hints at the end point at the module
# AFTER a reboot rather than at the install directory: $MODPATH points at
# /data/adb/modules_update/ during install, which does not work until a reboot.
MODID=$(sed -n 's/^id=//p' "$MODPATH/module.prop" 2>/dev/null | head -1)
[ -n "$MODID" ] || MODID=unfuse_zygisk

# --- module config -----------------------------------------------------------
#
# The setting (key path) ships inside the module and is read from there by
# storage.sh and path-mode.sh. The module directory is rewritten whole on every
# install, so the shipped default would wipe the user's choice: the mode is
# carried over from the previous install, and once from the old
# /data/adb/unfuse_zygisk.conf.
#
CONF="$MODPATH/unfuse_zygisk.conf"

# Mode found in a previous install: the module directory first, then the legacy
# /data/adb location.
prev_mode() {
    for f in "/data/adb/modules/$MODID/unfuse_zygisk.conf" \
             /data/adb/unfuse_zygisk.conf; do
        [ -r "$f" ] || continue
        m=$(sed -n 's/^[[:space:]]*path[[:space:]]*=[[:space:]]*//p' "$f" 2>/dev/null \
            | sed 's/[[:space:]]*#.*$//' | sed 's/[[:space:]]*$//' | tail -n 1)
        case "$m" in
            auto|sdcardfs|acl) printf '%s\n' "$m"; return 0 ;;
            raw)               printf 'acl\n';     return 0 ;;
        esac
    done
    return 1
}

if [ -f "$CONF" ] && prev="$(prev_mode)"; then
    sed -i "s|^[[:space:]]*path[[:space:]]*=.*|path=$prev|" "$CONF" 2>/dev/null
fi

# The legacy copy is no longer read by anything.
rm -f /data/adb/unfuse_zygisk.conf 2>/dev/null

# What was chosen in the end: the config value, or auto if there is none.
MODE=""
if [ -r "$CONF" ]; then
    MODE=$(sed -n 's/^[[:space:]]*path[[:space:]]*=[[:space:]]*//p' "$CONF" 2>/dev/null \
        | sed 's/[[:space:]]*#.*$//' | sed 's/[[:space:]]*$//' | tail -n 1)
fi
case "$MODE" in
    auto|sdcardfs|acl) ;;
    raw) MODE=acl ;;
    *) MODE=auto ;;
esac

ui_print "- Внутренняя память отдаётся приложениям напрямую:"
ui_print "  корень /storage/emulated, Android/data, Android/obb и каталоги"
ui_print "  чужих пакетов — как на Android 10 и раньше."

case "$MODE" in
    acl)
        ui_print "- Режим пути: acl — только сырое дерево /data/media с ACL на"
        ui_print "  группу 9997 (AID_EVERYBODY); sdcardfs не поднимается."
        ;;
    sdcardfs)
        ui_print "- Режим пути: sdcardfs — только основной путь. Если ядро его не"
        ui_print "  умеет, приложения останутся без памяти (альтернативный запрещён)."
        ;;
    *)
        if grep -qw sdcardfs /proc/filesystems 2>/dev/null; then
            ui_print "- Режим пути: auto — ядро умеет sdcardfs, будет основной путь."
        else
            ui_print "- Режим пути: auto — в ядре нет sdcardfs, включится"
            ui_print "  альтернативный: сырое дерево /data/media с ACL на группу 9997."
        fi
        ;;
esac

ui_print "- Режим и галочки состояния — в веб-интерфейсе модуля (WebUI в менеджере)."
ui_print "- Сменить режим из терминала:"
ui_print "  sh /data/adb/modules/$MODID/path-mode.sh acl|sdcardfs|auto"
ui_print "- Отключить: создать /data/adb/modules/$MODID/disable и перезагрузиться."
