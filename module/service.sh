#!/system/bin/sh
#
# service.sh — повтор подготовки после того, как vold смонтирует хранилища.
#
# vold при подготовке /data/media мог вернуть ярлык media_userdir_file, поэтому
# оба шага storage.sh повторяются уже после его старта. Шаги идемпотентны.
#

MODDIR=${MODDIR:-${0%/*}}
sh "$MODDIR/storage.sh" service
