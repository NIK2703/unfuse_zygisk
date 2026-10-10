#!/system/bin/sh
#
# post-fs-data.sh — prepare storage before Zygote starts.
#
# The module can only expose a storage source to an app inside its private
# namespace, so by the time the first app launches this must already exist:
#
#   * the media_rw_data_file label on the /data/media root;
#   * the /mnt/runtime/*/emulated mounts, which storage.sh raises as sdcardfs;
#   * on Android 11, persist.sys.fuse off — see below.
#
# storage.sh does all of that. It runs again from service.sh, after vold.
#

MODDIR=${MODDIR:-${0%/*}}
# Общие примитивы обеих сборок: проверка релиза и смена свойства.
. "$MODDIR/lib.sh"

# --- Android 11: форму хранилища выбирает булево свойство, и нам нужна sdcardfs
#
# 11 — единственный релиз, где FUSE и sdcardfs переключаются свойством
# persist.sys.fuse, и переключаются сразу в двух местах:
#
#   * vold: EmulatedVolume.cpp:304 `bool isFuse = GetBoolProperty(kPropFuse,
#     false)`. При true поверх sdcardfs монтируется FUSE-том, при false — нет;
#   * Zygote: Zygote.cpp:834 `if (isFuse)`. При true в /storage биндится
#     /mnt/user/<u> (том FUSE), при false — ExternalStorageViews[mode], то есть
#     /mnt/runtime/{default,read,write,full} (Zygote.cpp:329-339, 852-856) —
#     ровно те точки, которые поднимает storage.sh.
#
# С 12 ни той, ни другой ветки нет: там всегда FUSE, и свойство не читает никто
# (проверено по 30..37, byver/a3*). Поэтому весь блок — только SDK 30.
#
# AOSP ставит свойство в true: init.rc:792 в on post-fs-data делает
# `setprop persist.sys.fuse true` с комментарием "Enable FUSE by default".
# Модулю нужна обратная форма, поэтому здесь оно снимается.
#
# Порядок обязателен, и он наш. init.rc держит на свойстве триггеры
# (init.rc:797-808), которые биндят /storage в корневом namespace под
# zygote-start, а VolumeManager::linkPrimary() (VolumeManager.cpp:382-395) под
# !isFuse создаёт /mnt/user/<u>/primary — без него не разрешается /sdcard.
# Значит свойство должно стоять до zygote-start, и post-fs-data — единственное
# место. Скрипт запускается действием `on post-fs-data` из /system/etc/init,
# /system_ext/etc/init, /product/etc/init, /odm/etc/init или /vendor/etc/init, а
# эти каталоги init разбирает ПОСЛЕ /system/etc/init/hw/init.rc
# (init.cpp:277-291), и команды одного триггера идут в порядке разбора: наша
# строка оказывается после init.rc:792 и переживает её. resetprop -n значение не
# персистит — без модуля устройство возвращается к тому, что поставил вендор.
#
# external_storage.sdcardfs.enabled здесь НЕ трогается, и это осознанно. Им
# IsSdcardfsUsed() (Utils.cpp:1006-1009) решает, форкать ли /system/bin/sdcard
# (EmulatedVolume.cpp:308), а читается оно в КОНСТРУКТОРЕ EmulatedVolume — то
# есть при старте vold, а vold стартует в on early-fs (init.rc:451-453), до
# post-fs-data (init.rc:434). Поставить его отсюда — получить vold, который
# sdcard уже форкнул, но дальше считает, что sdcardfs нет, и лезет с ACL,
# квотами и fixupAppDir в дерево, которое отдаёт sdcardfs (Utils.cpp:141,254,
# 282,298,1568,1620; VolumeManager.cpp:784,1058). Оставляем как есть: при 0 точки
# наши, при 1 vold поднимет sdcardfs сам теми же масками (sdcard.cpp:178-189:
# 0006/1015, 0027/9997, 0007/9997, 0007/9997) и ляжет сверху. Что получилось —
# говорит sdcardfs_verify() в storage.sh, а не это свойство.
if unfuse_is_android_11 &&
   [ "$(getprop persist.sys.fuse)" != "false" ]; then
    unfuse_setprop persist.sys.fuse false
fi

# The native module logs one sample per boot and guards it with this marker, so
# it has to be gone before Zygote starts — otherwise the tag stays silent for
# the whole boot after the first one. Here, not in service.sh: that runs after
# apps are already launching.
rm -f /data/adb/unfuse_zygisk.once

sh "$MODDIR/storage.sh" post-fs-data
