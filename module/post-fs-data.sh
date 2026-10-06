#!/system/bin/sh
#
# post-fs-data.sh — подготовка хранилища до старта Zygote.
#
# Модуль умеет только подкладывать источник памяти приложению в приватный
# namespace, поэтому к моменту запуска первого приложения уже должны
# существовать:
#
#   * ярлык media_rw_data_file на корне /data/media;
#   * маунты /mnt/runtime/*/emulated — sdcardfs, если ядро его умеет, либо
#     (на альтернативном пути) ACL с группой 9997 на сыром дереве.
#
# Всё это делает storage.sh. Повтор — в service.sh, после vold.
#
# На альтернативном пути vold ещё допишет свои default-ACL: CE-хранилище
# пользователя он готовит позже этой стадии (vold-16/FsCrypt.cpp:657 — здесь
# только DE). Гонку закрывает не таймер и не патч, а сторож из storage.sh —
# см. tools/storage-fix.c и README §3.3.
#

MODDIR=${MODDIR:-${0%/*}}

sh "$MODDIR/storage.sh" post-fs-data
