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

MODDIR=${MODDIR:-${0%/*}}
LOG=/data/adb/sdcardfs_restore.log

sh "$MODDIR/storage.sh" post-fs-data

# --- снять с vold запись default-ACL для группы 1023 --------------------------
#
# vold готовит CE-хранилище пользователя позже этой стадии: здесь он готовит
# только DE (vold-16/FsCrypt.cpp:657), а CE отдаёт фреймворку — «the framework
# will prepare the user's CE storage later, once their CE key is installed».
# Значит, до его записи SetDefaultAcl(media_ce_path, ...) мы успеваем.
#
# Патч живёт в памяти процесса, поэтому после каждой перезагрузки его надо
# ставить заново: здесь и, на всякий случай, в service.sh. Оба шага
# идемпотентны — на свежем vold трамплин цел, на уже пропатченном утилита
# просто подтверждает, что патч на месте.
#
# Обоснование самого патча — в шапке tools/vold-noacl.c.
#
NOACL="$MODDIR/tools/vold-noacl"
if [ -x "$NOACL" ]; then
    "$NOACL" --wait 3 >>"$LOG" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: vold-noacl вернул $rc —" \
            "vold перепишет default-ACL у /data/media/0" >>"$LOG"
    fi
else
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: нет $NOACL —" \
        "vold перепишет default-ACL у /data/media/0" >>"$LOG"
fi
