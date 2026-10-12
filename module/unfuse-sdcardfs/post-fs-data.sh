#!/system/bin/sh
#
# post-fs-data.sh — prepare storage before Zygote starts.
#
# The module can only expose a storage source to an app inside its private
# namespace, so by the time the first app launches this must already exist:
#
#   * the media_rw_data_file label on the /data/media root;
#   * the /mnt/runtime/*/emulated mounts, which storage.sh raises as sdcardfs.
#
# storage.sh does all of that. It runs again from service.sh, after vold.
#
# persist.sys.fuse is NOT set by this script. The pair of Android 11 storage-form
# properties lives in system.prop — a file the module loader applies itself, and
# only after this script has returned. See below for both.

MODDIR=${MODDIR:-${0%/*}}

# --- Android 11: why persist.sys.fuse is not set here -------------------------
#
# 11 — единственный релиз, где FUSE и sdcardfs переключаются свойством
# persist.sys.fuse, и переключаются сразу в двух местах:
#
#   * vold: EmulatedVolume.cpp:304 `bool isFuse = GetBoolProperty(kPropFuse,
#     false)`. При true поверх sdcardfs монтируется FUSE-том, при false — нет;
#   * Zygote: Zygote.cpp:834 `if (isFuse)`. При true в /storage биндится
#     /mnt/user/<u> (том FUSE), при false — ExternalStorageViews[mode], то есть
#     /mnt/runtime/{default,read,write,full} (Zygote.cpp:329-338, 851-857).
#
# Раньше здесь стояло `resetprop -n persist.sys.fuse false`. Оно не работало, и
# работать не могло: порядок загрузки не оставляет модулю шанса. init.rc:792 в
# on post-fs-data ставит true; наш скрипт перекрывает его в памяти; затем
# late-init (init.rc:415) до zygote-start (init.rc:443) запускает
# load_persist_props (init.rc:405-406, 437), а тот читает
# /data/property/persistent_properties — где лежит true — и перезаписывает
# память. Замер по dmesg устройства: Magisk выходит из post-fs-data в 13.081 с,
# load_persist_props — в 13.136 (56 мс), ветка init.rc:797 по true — в 13.181.
# Записать в стор самому (resetprop -p) можно, но это уже не настройка модуля, а
# правка чужого значения: ROM или любой setprop вернут его обратно.
#
# Поэтому здесь свойство не трогается — модуль от него не зависит. Zygisk-плечо
# (src/unfuse_sdcardfs.cpp) кладёт sdcardfs в namespace приложения на точки
# ОБЕИХ веток Zygote сразу, и какая бы ветка ни сработала, источник лежит там,
# где она его ищет. Разбор — docs/android-11-persist-fuse-prop.md.
#
# --- и всё же свойство ставится: system.prop -----------------------------------
#
# Пара persist.sys.fflag.override.settings_fuse + persist.sys.fuse лежит в
# system.prop — файле рядом с этим скриптом. Ставит его загрузчик модулей, и
# ставит на этой же стадии, но ПОСЛЕ нас: Magisk — MagiskD::post_fs_data()
# (bootstages.cpp:307): handle_modules() со скриптами, затем load_modules() с
# system.prop (bootstages.cpp:349, module.cpp:282-286); KernelSU — так же
# (init_event.rs: exec_stage_script("post-fs-data"), затем load_system_prop()).
#
# Механизм тот же самый, что у снятой строки, — resetprop -n: запись мимо
# property_service, без персиста. Значит и судьба та же: если store уже содержит
# пару, load_persist_props вернёт её и system.prop останется заявкой на память.
# Разница только в том, что в system.prop лежит ОБА значения: система сравнивает
# их между собой на bootCompleted и при расхождении перезагружается один раз,
# после чего пара согласована. Подробно — docs/android-11-persist-fuse-prop.md, §5.2.

sh "$MODDIR/storage.sh"
