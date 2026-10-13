#!/system/bin/sh
# device-probe-android-view.sh [MODDIR] — диагност «пустого Android» для сборки
# unfuse (vold-fusefs): почему приложения не видят список /storage/emulated/0/
# Android/data.
#
# Запуск из-под root на устройстве (adb shell su -c ... или напрямую из-под root):
#   sh device-probe-android-view.sh [/data/adb/modules/unfuse_zygisk]
#
# Печатает ровно те факты, по которым различаются причины (см.
# docs/android-11-empty-android-dir.md):
#   1. поддержку POSIX ACL ядром (grant через gid 9997 против именованной записи);
#   2. наличие sdcardfs в ядре (vold поднимает свои виды по умолчанию);
#   3. вершину стека маунтов на /mnt/user/0/emulated (bind встал / F4);
#   4. что лежит на /mnt/user/0/emulated/0/Android/{data,obb} — вид sdcardfs
#      (0771 gid 1015, листинг app'ам закрыт) или ничего (сырое дерево);
#   5. режимы сырого /data/media/0/Android* (gid 9997 = награда на месте);
#   6. коды возврата storage-fix --check и vold-fusefs --check.
#
# Ничего не меняет — только читает. Модуль ничего не пишет; этот скрипт тоже.

MODDIR="${1:-/data/adb/modules/unfuse_zygisk}"
FIX="$MODDIR/tools/storage-fix"
FUSEFS="$MODDIR/tools/vold-fusefs"

# Верхний (последний) тип ФС на точке — маунты штабелируются, побеждает
# последний. /proc/mounts, а не stat: на стадиях модуля KernelSU подсовывает
# busybox, чей stat печатает UNKNOWN вместо магии.
top_type() {
    _t=""
    while read -r _dev mp ty _rest; do
        [ "$mp" = "$1" ] && _t="$ty"
    done < /proc/mounts 2>/dev/null
    [ -n "$_t" ] && echo "$_t" || echo "(не смонтировано)"
}

line() { echo "------------------------------------------------------------"; }

echo "=== 0. окружение ==="
echo "SDK=$(getprop ro.build.version.sdk)  ROM=$(getprop ro.build.version.incremental)"
echo "isolated_storage=$(getprop persist.sys.isolated_storage) (пусто = false, mAppDataIsolationEnabled)"
grep -qw sdcardfs /proc/filesystems 2>/dev/null && echo "sdcardfs в ядре: ЕСТЬ" || echo "sdcardfs в ядре: нет"
line

echo "=== 1. POSIX ACL в ядре (storage-fix --dump на корне тома) ==="
# Строка «POSIX ACL ядром не поддерживается» = грант едет на группе (gid 9997),
# её снимает любой vold'овский chown.
if [ -x "$FIX" ]; then
    "$FIX" --dump /data/media/0
else
    echo "storage-fix не найден: $FIX"
fi
line

echo "=== 2. вершина стека маунтов (init ns) ==="
for p in /mnt/user/0/emulated /mnt/pass_through/0/emulated \
         /mnt/user/0/emulated/0/Android/data /mnt/user/0/emulated/0/Android/obb; do
    echo "  $p -> $(top_type "$p")"
done
echo "  /storage/emulated/0 = $(stat -f -c %t /storage/emulated/0 2>/dev/null) (65735546 = fuse, f2f52010 = сырой бинд)"
line

echo "=== 3. сырое дерево /data/media/0/Android* ==="
ls -ld /data/media/0 /data/media/0/Android /data/media/0/Android/data \
      /data/media/0/Android/obb /data/media/0/Android/media 2>&1
echo "gid 9997 на уровнях = награда модуля на месте"
line

echo "=== 4. коды возврата ==="
if [ -x "$FIX" ]; then
    "$FIX" --check /data/media/0 /data/media/0/Android >/dev/null 2>&1
    echo "storage-fix --check /data/media/0 /data/media/0/Android = $? (0 = награда цела)"
fi
if [ -x "$FUSEFS" ]; then
    "$FUSEFS" --check >/dev/null 2>&1
    echo "vold-fusefs --check = $? (0 = патч в памяти vold)"
else
    echo "vold-fusefs не найден: $FUSEFS"
fi
line

echo "=== 5. как читать ==="
cat <<'TXT'
  * /storage/emulated/0 = fuse          -> F4: патч vold не встал, лечить надо
    причину (vold-fusefs), а не дерево.
  * .../Android/data -> sdcardfs        -> вид sdcardfs default (0771 gid 1015)
    прячет сырое дерево: app'ам other=--x, листинга нет. После исправления этой
    строки быть НЕ должно (модуль снимает бинд в service.sh).
  * .../Android/data -> (не смонтировано)  -> app-путь ведёт на сырое дерево;
    листинг решают режимы из §3 (gid 9997) и ACL.
  * gid != 9997 в §3                      -> награда сбита vold'овским chown.
TXT
exit 0
