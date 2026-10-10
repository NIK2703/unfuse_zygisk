#!/system/bin/sh
#
# storage.sh — relabel /data/media and give apps ACLs on the raw tree (from
# post-fs-data.sh and service.sh). With neither sdcardfs nor FUSE handing out
# permissions, the ACL for group 9997 is what grants access; the per-package
# entry for the app's own uid is what keeps an app in its own Android/data/<pkg>
# when the object there was created by another process.
#
# Диагностический лог — включён по умолчанию, глушится /data/adb/unfuse_zygisk.nolog
# (module/log.sh).

MODDIR="${MODDIR:-${0%/*}}"
. "$MODDIR/log.sh"
# Общие примитивы обеих сборок: перемаркировка корня /data/media.
. "$MODDIR/lib.sh"

# media_userdir_file gives appdomain one search and no getattr (domain.te:252),
# so stat()/ls() on /storage/emulated return EACCES with no AVC; the contents
# already carry media_rw_data_file, where appdomain has full rights (app.te:149).
# Почему именно так и что означают коды возврата — в lib.sh
# (unfuse_relabel_media_root).
unfuse_relabel_media_root >/dev/null
# Метку не печатаем: её печатает снимок («--- модуль ---», строка «метка
# /data/media»). Здесь осталось только само действие.

# /data/media is 1023:1023 and apps are not in group 1023, so FUSE handed out
# 0770/0660 for gid 9997 (AID_EVERYBODY, android_filesystem_config.h:166).
#
# Named entries, not chmod: vold resets owner and mode of /data/media and the
# Android* dirs every boot, and chmod touches only USER_OBJ/GROUP_OBJ/MASK/
# OTHER; a default ACL also makes the kernel skip umask. vold appends default
# ACLs after the service stage — tools/vold-noacl.c stops that.
FIX="$MODDIR/tools/storage-fix"
[ -x "$FIX" ] || { log "storage.sh: нет $FIX"; exit 1; }

run_logged "storage-fix --traverse /data/media" "$FIX" --traverse /data/media

# /data/media/0 is absent on a not-yet-expanded volume; service.sh retries.
# Отсутствие каталога печатает снимок («dump /data/media/0: нет»); здесь — только
# решение: проход пропущен.
if [ -d /data/media/0 ]; then
    run_logged "storage-fix /data/media/0" "$FIX" /data/media/0
else
    log "storage.sh: /data/media/0 пропущен (нет каталога)"
fi

# The per-package entry, one level deep. vold writes this ACL itself
# (SetDefaultAcl with additionalGids = {uid}, vold-11/Utils.cpp:321,:398) and
# tools/vold-noacl suppresses that write, so the module has to supply it —
# without it an object created inside Android/data/<pkg> by another uid (a root
# helper, an installer, MTP) leaves the app with no matching entry at all, and a
# restrictive mode from the creator cuts the mask as well. Depth 0 of the
# application-specific directory is the only place vold writes it, and anything
# deeper inherits the default ACL from there, so this pass stops at one level.
if [ -d /data/media/0/Android ]; then
    for d in data obb media; do
        if [ -d "/data/media/0/Android/$d" ]; then
            run_logged "storage-fix --app-dirs /data/media/0/Android/$d" \
                "$FIX" --app-dirs "/data/media/0/Android/$d"
        fi
    done
fi

if [ -d /data/media/obb ]; then
    run_logged "storage-fix /data/media/obb" "$FIX" /data/media/obb
fi

exit 0
