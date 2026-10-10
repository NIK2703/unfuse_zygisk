#!/system/bin/sh
# diag.sh — снимок состояния доступа к /data/media. Ничего не меняет
# (единственное исключение — --probe: создаёт и сразу удаляет один файл).
#
#   sh diag.sh                          снимок в stdout
#   sh diag.sh --log                    то же плюс дописать в $UNFUSE_LOG
#   sh diag.sh --probe <пакет>          проба доступа от uid приложения
#   sh diag.sh --path <путь> [<uid>]    разбор произвольного пути по компонентам
#                                       (+ проба записи, если дан uid)
#
# Зачем он есть. EACCES у приложения не виден снаружи: `ls` не показывает ACL,
# `stat` не показывает маску, а ошибка на глубоком пути не говорит, какой
# каталог отказал. Разбор логов от 2026-10-09 (два приложения не могли создать
# файл в своём же Android/data/<pkg>) упирался ровно в это — в логах был только
# errno, а состояния дерева на том устройстве никто не снял. Здесь собрано всё,
# чего тогда не хватило: форма хранилища, состояние обоих патчей vold, режимы и
# ACL всей цепочки Android*, состав групп живых процессов приложений и проба,
# повторяющая сисколл приложения с его uid И его дополнительными группами.
#
# Правило одно: каждое данное попадает в лог ровно один раз.
#   * состояние (что сейчас на устройстве) принадлежит снимку — этот файл;
#   * действия, коды возврата и решения (что сделали и по какой ветке) —
#     скриптам (post-fs-data.sh, service.sh, storage.sh). Скрипт НЕ повторяет
#     значение, которое печатает снимок: он говорит «пропуск (патч не встал)», а
#     не «пропуск (--check=1)», потому что --check уже стоит строкой выше.
#   * один путь дампится один раз за снимок (dump_once), одна цель пробуется
#     один раз (probe_once) — разделы --path и --probe пересекаются.
#   * один объект — один раздел: сырое дерево и то, что обслуживает /storage,
#     лежат вместе, в «--- хранилище ---», и форма /storage/emulated/0 приходит
#     либо магией ФС, либо строкой монтирования, но не обеими сразу.
# Прежде всё это печаталось дважды, и по логу было не понять, какое из двух
# значений — про то же самое или про разные вещи.

MODDIR=${MODDIR:-${0%/*}}
. "$MODDIR/log.sh"

TO_LOG=0
PROBE_PKG=
PATH_ARG=
PATH_UID=

while [ $# -gt 0 ]; do
    case "$1" in
        --log)   TO_LOG=1 ;;
        --probe) shift; PROBE_PKG=$1 ;;
        --path)  shift; PATH_ARG=$1 ;;
        --uid)   shift; PATH_UID=$1 ;;
        *)       echo "usage: $0 [--log] [--probe <package>] [--path <путь> [--uid <uid>]]" >&2; exit 2 ;;
    esac
    shift
done

# Снимок собирается в файл целиком, а не печатается по ходу. Пишется всегда
# полностью — никаких сравнений с прошлым состоянием.
BUF=${TMPDIR:-/data/local/tmp}/unfuse-diag.$$.txt
: >"$BUF" 2>/dev/null || { echo "некуда писать $BUF" >&2; exit 1; }
trap 'rm -f "$BUF"' EXIT INT TERM

# put — единственный выход наружу: stdout и (с --log) лог. В буфер не попадает:
# шапка стадии и дата меняются всегда, и в различиях они были бы чистым шумом.
put() {
    printf '%s\n' "$*"
    [ "$TO_LOG" = "1" ] && printf '%s\n' "$*" >>"$UNFUSE_LOG" 2>/dev/null
    return 0
}

buf() { printf '%s\n' "$*" >>"$BUF"; return 0; }

# Вывод вспомогательных утилит идёт в буфер: прямой вызов печатал бы в stdout, а
# автозапуск (unfuse_diag) гасит stdout в /dev/null — в логе остались бы только
# строки emit. Именно так первый прогон потерял все --dump и список монтирований.
# rc отдельной строкой — только там, где он значит (--check, --probe): у --dump
# он всегда 0, и строка была бы повтором на каждом из 62 каталогов.
buf_out() {
    _o=$("$@" 2>&1)
    [ -n "$_o" ] && printf '%s\n' "$_o" | while IFS= read -r _l; do buf "$_l"; done
    return 0
}

buf_cmd() {
    _tag=$1; shift
    _o=$("$@" 2>&1); _rc=$?
    [ -n "$_o" ] && printf '%s\n' "$_o" | while IFS= read -r _l; do buf "$_l"; done
    buf "$_tag: rc=$_rc"
    return 0
}

# Один путь — один дамп за снимок. Разделы перекрываются: --path идёт по
# компонентам пути, --probe — по <pkg>/cache, и один и тот же каталог попадал в
# лог дважды, а «dump <путь>: нет» — тоже.
#
# Ключ — буквальный путь, БЕЗ приведения /storage/emulated/0 к /data/media/0:
# при живом FUSE это разные объекты (вид приложения и сырое дерево), и свести их
# значило бы стереть ровно то различие, ради которого снимок и снимается.
# Поэтому два написания одного дерева в логе остаются — это не повтор, а два
# измерения; повторяется здесь только буквально один и тот же путь.
DUMPED=
dump_once() {
    case " $DUMPED " in *" $1 "*) return 0 ;; esac
    DUMPED="$DUMPED $1"
    if [ -e "$1" ]; then
        buf_out "$FIX" --dump "$1"
    else
        buf "dump $1: нет"
    fi
    return 0
}

# Живой процесс с этим uid — из него берётся mount-namespace приложения.
pid_of_uid() {
    for _p in $(ls -1 /proc 2>/dev/null | grep -E '^[0-9]+$'); do
        _u=$(awk '/^Uid:/{print $2; exit}' "/proc/$_p/status" 2>/dev/null)
        [ "$_u" = "$1" ] && { echo "$_p"; return 0; }
    done
    return 1
}

# Проба записи по одному пути: сначала в корневом namespace, затем — если известен
# живой процесс приложения — внутри namespace этого процесса. Разница между двумя
# прогонами и есть ответ: если отказ виден только в namespace приложения, дело в
# монтировании, а не в правах дерева. nsenter есть в toybox (/system/bin/nsenter).
probe_at() {
    _f=$1; _u=$2; _pid=$3
    buf_cmd "probe $_f [root]" "$FIX" --probe "$_u" "$_f"
    if [ -n "$_pid" ] && [ -x /system/bin/nsenter ]; then
        # `--` обязателен: без него toybox-nsenter разбирает `--probe` как свой
        # ключ и падает с "Unknown option".
        buf_cmd "probe $_f [ns $_pid]" /system/bin/nsenter -t "$_pid" -m -- "$FIX" --probe "$_u" "$_f"
    fi
    return 0
}

# То же по цели пробы: один и тот же путь не пробуется дважды (--path и --probe
# могут сойтись на одном каталоге).
PROBED=
probe_once() {
    case " $PROBED " in *" $1 "*) return 0 ;; esac
    PROBED="$PROBED $1"
    probe_at "$1" "$2" "$3"
    return 0
}

FIX="$MODDIR/tools/storage-fix"
[ -x "$FIX" ] || { echo "нет $FIX" >&2; exit 1; }
# /data/adb/unfuse_zygisk.state: MODDIR — это /data/adb/modules/<id>, то есть два
# уровня вниз. Раньше здесь стояло `$MODDIR/../…` — путь не существовал, и
# строка «состояние:» в снимке всегда была пустой.
STATE=$(dirname "$(dirname "$MODDIR")")/unfuse_zygisk.state

# ---------------------------------------------------------------- шапка
put ""
put "######## снимок: ${UNFUSE_STAGE:-ручной запуск} ########"
put "дата:      $(date)"

# ---------------------------------------------------------------- сборка
buf ""
buf "--- модуль ---"
buf "ядро:      $(uname -r 2>/dev/null)"
buf "sdk:       $(getprop ro.build.version.sdk) (Android $(getprop ro.build.version.release))"
buf "устройство: $(getprop ro.product.model) / $(getprop ro.build.display.id)"
buf "selinux:   $(getenforce 2>/dev/null)"
buf "версия:    $(sed -n 's/^version=//p' "$MODDIR/module.prop" 2>/dev/null)"
buf "fuse:      persist.sys.fuse=$(getprop persist.sys.fuse)"
buf "sdcardfs:  external_storage.sdcardfs.enabled=$(getprop external_storage.sdcardfs.enabled)"
# Метки — здесь и только здесь. Раньше их печатали скрипты (post-fs-data после
# mkdir/chcon state, storage.sh после chcon /data/media) — то есть то же самое
# уходило в лог дважды. Метка это состояние, а состояние принадлежит снимку.
buf "метка /data/media: $(ls -Zd /data/media 2>/dev/null | awk '{print $1}')"
buf "метка state:       $(ls -Zd "$STATE" 2>/dev/null | awk '{print $1}')"
if [ -f "$UNFUSE_NOLOG" ]; then
    buf "логирование: выкл ($UNFUSE_NOLOG)"
else
    buf "логирование: вкл"
fi
buf "лог:       $UNFUSE_LOG"
buf "состояние: $(ls -1 "$STATE" 2>/dev/null | tr '\n' ' ')"
for f in "$MODDIR/tools/"* "$MODDIR/zygisk/"*.so; do
    [ -f "$f" ] || continue
    buf "$(md5sum "$f" 2>/dev/null)"
done

# ---------------------------------------------------------------- хранилище
# Один объект — один раздел. Раньше их было два («форма хранилища» и «mounts
# (emulated)»), и форма /storage/emulated/0 попадала в лог дважды: магией ФС и
# строкой монтирования. Теперь у каждой стороны свой владелец:
#   сырое дерево   — магией ФС (у него нет записи в таблице монтирований);
#   что обслуживает /storage — таблицей монтирований, и только ею.
buf ""
buf "--- хранилище ---"
for p in /data/media /data/media/0 /data/media/obb; do
    v=$(stat -f -c %t "$p" 2>/dev/null)
    buf "$(printf '%-24s' "$p") fs=${v:-—}"
done

emulated=$(grep -E 'emulated' /proc/mounts 2>/dev/null)
if [ -n "$emulated" ]; then
    printf '%s\n' "$emulated" | while IFS= read -r _l; do buf "$_l"; done
else
    buf "нет"
fi

# Магия /storage/* — только там, где строки монтирования НЕТ. Иначе одно и то же
# уходило бы в лог дважды. Именно этот случай (монтирования ещё нет, путь лежит
# внутри tmpfs) и был тем, где 1021994 = 0x01021994 читали как «десять
# миллионов»: строка монтирования тут отсутствует, и без магии случай был бы
# неотличим от «нет такого пути».
for p in /storage/emulated /storage/emulated/0; do
    grep -qE "[[:space:]]$p[[:space:]]" /proc/mounts 2>/dev/null && continue
    v=$(stat -f -c %t "$p" 2>/dev/null)
    buf "$(printf '%-24s' "$p") fs=${v:-—} (монтирования нет)"
done

# ---------------------------------------------------------------- vold
buf ""
buf "--- vold и патчи ---"
buf "pid vold:            $(pidof vold 2>/dev/null)"
buf "init.svc_debug_pid.vold: $(getprop init.svc_debug_pid.vold)"
buf "ro.boottime.vold:        $(getprop ro.boottime.vold)"
buf_cmd "vold-fusefs --check" "$MODDIR/tools/vold-fusefs" --check
buf_cmd "vold-noacl --check" "$MODDIR/tools/vold-noacl" --check

# ---------------------------------------------------------------- дерево
buf ""
buf "--- Android* ---"
for p in /data/media /data/media/0 /data/media/0/Android /data/media/0/Android/data \
         /data/media/0/Android/obb /data/media/0/Android/media /data/media/obb; do
    dump_once "$p"
done
buf_cmd "check Android/data" "$FIX" --check /data/media/0/Android/data

buf ""
buf "--- каталоги пакетов ---"
count=0
for d in data obb media; do
    base="/data/media/0/Android/$d"
    [ -d "$base" ] || continue
    for pkg in "$base"/*; do
        [ -d "$pkg" ] || continue
        dump_once "$pkg"
        count=$((count + 1))
    done
done
buf "каталогов пакетов: $count"

# ---------------------------------------------------------------- процессы
buf ""
buf "--- процессы приложений ---"
shown=0
for pid in $(ls -1 /proc 2>/dev/null | grep -E '^[0-9]+$'); do
    uid=$(stat -c %u "/proc/$pid" 2>/dev/null)
    case "$uid" in ''|*[!0-9]*) continue ;; esac
    [ "$uid" -ge 10000 ] 2>/dev/null || continue

    name=$(sed -n 's/^Name:[[:space:]]*//p' "/proc/$pid/status" 2>/dev/null)
    groups=$(sed -n 's/^Groups:[[:space:]]*//p' "/proc/$pid/status" 2>/dev/null)
    buf "pid=$pid uid=$uid name=$name groups=$groups"

    seen=$(awk '$2 ~ /^\/storage\/emulated/ {printf "%s=%s ", $2, $3}' "/proc/$pid/mounts" 2>/dev/null)
    if [ -n "$seen" ]; then
        buf "  mounts: $seen"
    else
        buf "  mounts: нет"
    fi

    shown=$((shown + 1))
    [ "$shown" -ge 12 ] && break
done
[ "$shown" -eq 0 ] && buf "нет"

# ---------------------------------------------------------------- путь
# Произвольный путь из чужого лога: печатаются ВСЕ его компоненты. Отказ на
# глубоком пути не говорит, какой каталог отказал, а `cache/` на уровень ниже
# `<pkg>` в разделе каталогов пакетов не виден — именно там и падают оба
# приложения из логов пользователя.
if [ -n "$PATH_ARG" ]; then
    buf ""
    buf "--- путь: $PATH_ARG ---"
    _acc=
    _rest=$PATH_ARG
    while [ -n "$_rest" ]; do
        case "$_rest" in /*) _acc=; _rest=${_rest#/} ;; esac
        _c=${_rest%%/*}
        [ -z "$_c" ] && break
        _acc="$_acc/$_c"
        case "$_rest" in */*) _rest=${_rest#*/} ;; *) _rest= ;; esac
        dump_once "$_acc"
    done

    if [ -n "$PATH_UID" ]; then
        _pid=$(pid_of_uid "$PATH_UID")
        buf "uid=$PATH_UID pid=${_pid:-нет}"
        probe_once "$PATH_ARG" "$PATH_UID" "$_pid"
    fi
fi

# ---------------------------------------------------------------- проба
if [ -n "$PROBE_PKG" ]; then
    buf ""
    buf "--- проба: $PROBE_PKG ---"

    # uid пакета. `cmd package list packages -U` — стабильный формат; dumpsys
    # оставлен запасным: на Android 16 он печатает appId=, на Android 11 userId=,
    # и первая версия разбора молча давала «uid=нет» на телефоне.
    uid=$(cmd package list packages -U "$PROBE_PKG" 2>/dev/null \
          | awk -F'[ :]' -v p="$PROBE_PKG" '$1 == "package" && $2 == p { print $4; exit }')
    if [ -z "$uid" ]; then
        uid=$(dumpsys package "$PROBE_PKG" 2>/dev/null \
              | sed -n -e 's/.*userId=\([0-9]*\).*/\1/p' -e 's/.*appId=\([0-9]*\).*/\1/p' | head -1)
    fi
    if [ -z "$uid" ]; then
        buf "uid=нет"
    else
        pid=$(pid_of_uid "$uid")
        buf "uid=$uid pid=${pid:-нет}"

        for d in data obb media; do
            p="/storage/emulated/0/Android/$d/$PROBE_PKG"
            if [ -d "$p" ]; then
                probe_once "$p/.unfuse-probe" "$uid" "$pid"

                # Подкаталоги: провал приложений из логов — ровно здесь
                # (Android/data/<pkg>/cache/am.log). Дамп дедуплицирован
                # (dump_once): если --path уже прошёл этот каталог, второй раз он
                # в лог не попадёт.
                for sub in cache files no_backup; do
                    [ -d "$p/$sub" ] || continue
                    dump_once "$p/$sub"
                    probe_once "$p/$sub/.unfuse-probe" "$uid" "$pid"
                done
            else
                buf "probe $p: нет"
            fi
        done

        # Запись прямо в родителя — прокси для «приложение создаёт свой каталог»:
        # создать Android/data/<pkg> можно только имея w на Android/data.
        for d in data obb media; do
            p="/storage/emulated/0/Android/$d"
            [ -d "$p" ] || continue
            probe_once "$p/.unfuse-probe-$PROBE_PKG" "$uid" "$pid"
        done
    fi
fi

# ---------------------------------------------------------------- вывод
# Снимок ВСЕГДА полный. Никаких различий и сравнений со старым состоянием: лог
# просто перезаписывается текущим состоянием устройства. Опорный снимок и diff
# убраны (см. запрос пользователя) — хранить старый снимок и сравнивать его со
# свежим не нужно, это только добавляло строки «изменений нет» вместо состояния.
cat "$BUF" | while IFS= read -r _l; do put "$_l"; done
