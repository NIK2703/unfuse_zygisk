#!/system/bin/sh
# post-fs-data.sh — storage before Zygote: state dir, storage.sh, the vold FUSE
# patch, vold-noacl. Design: docs/fuse-root-patch-design.md.

MODDIR=${MODDIR:-${0%/*}}

# --- Android 11: форма хранилища
#
# 11 — единственный релиз, где FUSE-vs-sdcardfs решает булево свойство:
# EmulatedVolume.cpp:304 `bool isFuse = GetBoolProperty(kPropFuse, false)` и
# `if (isFuse && isVisible)` вокруг MountUserFuse(); Zygote.cpp:834 `if (isFuse)`
# с веткой else на ExternalStorageViews. С 12 обеих веток нет — там всегда FUSE
# (проверено по 30..37, docs/android-11-design.md).
#
# При false vold не зовёт MountUserFuse(), наш перехватчик mount() не
# срабатывает ни разу, а вид приложения собирается из /mnt/runtime/<view> —
# места sdcardfs. Модуль обслуживает только FUSE-форму, поэтому выставляем её.
# Это не своя политика, а значение AOSP по умолчанию: init.rc:792 в on
# post-fs-data делает ровно `setprop persist.sys.fuse true` («Enable FUSE by
# default»); вендор, у которого свойство false, эту строку снял. Порядок
# обязателен: триггеры init.rc:797-808 биндят /storage в корневом namespace под
# zygote-start по этому же свойству, значит свойство должно стоять до
# zygote-start, и post-fs-data — единственное для этого место.
if [ "$(getprop ro.build.version.sdk)" = "30" ]; then
    if [ "$(getprop persist.sys.fuse)" != "true" ]; then
        if command -v resetprop >/dev/null 2>&1; then
            resetprop -n persist.sys.fuse true
        else
            setprop persist.sys.fuse true
        fi
    fi

    # Своим условием, а не в блоке выше: другое свойство, другая половина задачи.
    # IsSdcardfsUsed() (Utils.cpp:1006) = sdcardfs в /proc/filesystems И это
    # свойство (умолчание true). Ноль приводит живые чтения vold к A12+:
    # quota-наследование (Utils.cpp:298,:404), AID_EXT_DATA_RW у Android/data
    # (:1620), сырой источник /mnt/pass_through (:1568).
    #
    # mUseSdcardFs этим свойством НЕ управляется и из модуля недостижим:
    # IsSdcardfsUsed() читается в конструкторе EmulatedVolume (:52), а тот живёт
    # в VolumeManager::start() (main.cpp:98) — до регистрации binder-сервиса
    # (:112), которую уже требует первая команда on post-fs-data (блокирующий
    # `exec vdc checkpoint prepareCheckpoint`, init.rc:531; vdc ждёт сервис
    # 5000×10 мс, vdc.cpp:34-46). Где в ядре есть sdcardfs, он остаётся true:
    # /system/bin/sdcard форкается (:308), и mountFuseBindMounts() берёт
    # Android/data и obb из /mnt/runtime/default/emulated/<u>/Android (:113).
    #
    # resetprop, а не setprop: external_storage. не объявлен в property_contexts
    # и падает в catch-all `* u:object_r:default_prop:s0` (:114) — не system_prop.
    if [ "$(getprop external_storage.sdcardfs.enabled)" != "0" ]; then
        if command -v resetprop >/dev/null 2>&1; then
            resetprop -n external_storage.sdcardfs.enabled 0
        else
            setprop external_storage.sdcardfs.enabled 0
        fi
    fi
fi

# `once` — boot marker the first app with storage claims; `no_hooks` — switch
# for libc patching. Not /data/adb: the zygote domain gets traversal only there
# and open(O_CREAT) fails; under magisk_file any domain may write. mkdir, not
# rm -rf — no_hooks is the user's and must survive a reboot.
STATE=/data/adb/unfuse_zygisk.state
mkdir -p "$STATE" 2>/dev/null
chcon u:object_r:magisk_file:s0 "$STATE" 2>/dev/null

# Must be gone before Zygote starts, or it stays absent for the whole boot.
rm -f "$STATE/once" 2>/dev/null

sh "$MODDIR/storage.sh"

# vold-fusefs redirects vold's mount() trampoline, so MountUserFuse() ends with
# a bind of raw /data/media over the FUSE mount. That mount stays — its fd is
# MediaProvider's contract — but nothing reaches it. umount2 goes in the same
# run: vold's teardown removes one mount per path, so without it our bind comes
# off and every later mount fails with ENOTCONN.
FUSEFS="$MODDIR/tools/vold-fusefs"
[ -x "$FUSEFS" ] && "$FUSEFS" --wait 3 >/dev/null 2>&1

# vold does only DE this early (vold-16/FsCrypt.cpp:657), so we run before its
# SetDefaultAcl(media_ce_path, ...). After the FUSE patch: the redirection
# removes the mount() calls the ACL pass needs.
NOACL="$MODDIR/tools/vold-noacl"
[ -x "$NOACL" ] && "$NOACL" --wait 3 >/dev/null 2>&1
