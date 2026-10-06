#!/system/bin/sh
#
# post-fs-data.sh — подготовка хранилища до старта Zygote.
#
# Модуль умеет только подкладывать sdcardfs приложению в приватный namespace,
# поэтому к моменту запуска первого приложения уже должны существовать:
#
#   * ярлык media_rw_data_file на корне /data/media;
#   * sdcardfs-маунты на /mnt/runtime/*/emulated.
#
# Всё это делает storage.sh. Повтор — в service.sh, после vold.
#

MODDIR=${MODDIR:-${0%/*}}
sh "$MODDIR/storage.sh" post-fs-data
