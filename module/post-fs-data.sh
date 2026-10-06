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
LOG=/data/adb/sdcardfs_restore.log

sh "$MODDIR/storage.sh" post-fs-data

# --- снять с vold запись default-ACL для группы 1023 --------------------------
#
# vold готовит CE-хранилище пользователя позже этой стадии: здесь он готовит
# только DE (vold-16/FsCrypt.cpp:657), а CE отдаёт фреймворку — «the framework
# will prepare the user's CE storage later, once their CE key is installed».
# Значит, до его записи мы успеваем. Обоснование самого патча — в исходнике
# tools/vold-noacl.c.
#
# Патч живёт в памяти процесса, поэтому после каждой перезагрузки его надо
# ставить заново: здесь и, на всякий случай, в service.sh. Оба шага
# идемпотентны.
#
# Утилита ничего не пишет, если vold оказался другой сборки: тогда она вернёт 2,
# и service.sh отработает по-старому — повторными проходами ACL.
NOACL="$MODDIR/tools/vold-noacl"
if [ -x "$NOACL" ]; then
    "$NOACL" --wait 3 >>"$LOG" 2>&1
    rc=$?
    [ "$rc" -eq 0 ] || echo \
        "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: vold-noacl вернул $rc —" \
        "повторные проходы ACL останутся включены" >>"$LOG"
else
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: нет $NOACL —" \
        "повторные проходы ACL останутся включены" >>"$LOG"
fi
