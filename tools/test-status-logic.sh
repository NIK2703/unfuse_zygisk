#!/bin/sh
# test-status-logic.sh — tests two status.sh blocks against stubbed data. Blocks are
# cut from status.sh by markers, so the test can't drift from the check. Only
# fs_type/mount_opts/logcat/module_mapped are stubbed.

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
STATUS=${STATUS:-$HERE/../module/status.sh}

[ -r "$STATUS" ] || { echo "нет $STATUS"; exit 2; }

SDCARDFS_FS=sdcardfs
POINTS="default:6:1015 read:23:9997 write:7:9997 full:7:9997"
NO_HOOKS=/nonexistent/no_hooks

kv() { echo "$1=$2"; }

has_opt() {
    case ",$1," in
        *",$2,"*) return 0 ;;
        *)        return 1 ;;
    esac
}

# Point -> name (default/read/write/full) so fixtures can be set by name.
point_name() {
    n="${1#/mnt/runtime/}"
    printf '%s' "${n%%/*}"
}

fs_type() {
    eval "printf '%s' \"\${STUB_FS_$(point_name "$1"):-}\""
}

mount_opts() {
    eval "printf '%s' \"\${STUB_OPT_$(point_name "$1"):-}\""
}

logcat() { [ -n "$STUB_LOGCAT" ] && printf '%s\n' "$STUB_LOGCAT"; return 0; }

module_mapped() { [ "$STUB_MAPPED" = 1 ]; }

# Blocks come from status.sh itself.
MO=$(sed -n '/^opts_ok=1$/,/^kv mount_opts /p' "$STATUS")
LH=$(sed -n '/^hooks=0$/,/^kv libc_hooks /p' "$STATUS")
[ -n "$MO" ] || { echo "не нашёл блок опций маунтов в $STATUS"; exit 2; }
[ -n "$LH" ] || { echo "не нашёл блок правки libc в $STATUS"; exit 2; }

fail=0
check() {
    if [ "$2" = "$3" ]; then
        printf '  ок    %-52s -> %s\n' "$1" "$2"
    else
        printf '  ПЛОХО %-52s -> ждали %s, получили %s\n' "$1" "$2" "$3"
        fail=1
    fi
}

echo "== опции маунтов =="

OPTS_GOOD_default="rw,nosuid,nodev,noexec,relatime,fsuid=1023,fsgid=1023,mask=6,gid=1015"
OPTS_GOOD_read="rw,nosuid,nodev,noexec,relatime,fsuid=1023,fsgid=1023,mask=23,gid=9997"
OPTS_GOOD_write="rw,nosuid,nodev,noexec,relatime,fsuid=1023,fsgid=1023,mask=7,gid=9997"
OPTS_GOOD_full="rw,nosuid,nodev,noexec,relatime,fsuid=1023,fsgid=1023,mask=7,gid=9997"

all_sd() {
    STUB_FS_default=sdcardfs; STUB_FS_read=sdcardfs
    STUB_FS_write=sdcardfs;   STUB_FS_full=sdcardfs
}

# 1. All as requested.
all_sd
STUB_OPT_default=$OPTS_GOOD_default; STUB_OPT_read=$OPTS_GOOD_read
STUB_OPT_write=$OPTS_GOOD_write;     STUB_OPT_full=$OPTS_GOOD_full
check "все четыре маунта, mask и gid совпали" "mount_opts=1" "$(eval "$MO")"

# 2. No mounts at all — used to pass silently.
STUB_FS_default=""; STUB_FS_read=""; STUB_FS_write=""; STUB_FS_full=""
STUB_OPT_default=""; STUB_OPT_read=""; STUB_OPT_write=""; STUB_OPT_full=""
check "точек нет вовсе" "mount_opts=0" "$(eval "$MO")"

# 3. Our mounts, but the kernel prints no options.
all_sd
STUB_OPT_default="rw,nosuid,nodev"; STUB_OPT_read="rw,nosuid,nodev"
STUB_OPT_write="rw,nosuid,nodev";   STUB_OPT_full="rw,nosuid,nodev"
check "ядро не печатает mask и gid" "mount_opts=0" "$(eval "$MO")"

# 4. Foreign mask on one point.
all_sd
STUB_OPT_default=$OPTS_GOOD_default; STUB_OPT_read=$OPTS_GOOD_read
STUB_OPT_write="rw,fsuid=1023,fsgid=1023,mask=6,gid=9997"; STUB_OPT_full=$OPTS_GOOD_full
check "у write чужая маска" "mount_opts=0" "$(eval "$MO")"

# 5. Foreign gid on one point.
all_sd
STUB_OPT_default=$OPTS_GOOD_default; STUB_OPT_read=$OPTS_GOOD_read
STUB_OPT_write=$OPTS_GOOD_write;     STUB_OPT_full="rw,fsuid=1023,fsgid=1023,mask=7,gid=1015"
check "у full чужой gid" "mount_opts=0" "$(eval "$MO")"

# 6. One point unmounted.
all_sd
STUB_FS_full=""
STUB_OPT_default=$OPTS_GOOD_default; STUB_OPT_read=$OPTS_GOOD_read
STUB_OPT_write=$OPTS_GOOD_write;     STUB_OPT_full=""
check "одна точка снята" "mount_opts=0" "$(eval "$MO")"

# 7. Only some points print options, but all shown match.
all_sd
STUB_OPT_default=$OPTS_GOOD_default
STUB_OPT_read="rw,nosuid,nodev"; STUB_OPT_write="rw,nosuid,nodev"; STUB_OPT_full="rw,nosuid,nodev"
check "печатает одна точка, значение верное" "mount_opts=1" "$(eval "$MO")"

# 8. gid=1015 must not match fsgid=1015.
all_sd
STUB_OPT_default="rw,fsuid=1023,fsgid=1015,mask=6"; STUB_OPT_read="rw,fsgid=9997,mask=23"
STUB_OPT_write="rw,fsgid=9997,mask=7";              STUB_OPT_full="rw,fsgid=9997,mask=7"
check "gid подменён на fsgid" "mount_opts=0" "$(eval "$MO")"

# 9. Kernel prints mask but not gid: no evidence for gid.
all_sd
STUB_OPT_default="rw,mask=6"; STUB_OPT_read="rw,mask=23"
STUB_OPT_write="rw,mask=7";   STUB_OPT_full="rw,mask=7"
check "печатает только mask" "mount_opts=0" "$(eval "$MO")"

echo
echo "== правка входов libc =="

all_sd
STUB_MAPPED=0
STUB_LOGCAT=""
NO_HOOKS=/nonexistent/no_hooks
check "строк нет, модуль не отображён" "libc_hooks=0" "$(eval "$LH")"

STUB_MAPPED=1
check "строк нет, но модуль отображён (сырой путь)" "libc_hooks=1" "$(eval "$LH")"

STUB_MAPPED=0
STUB_LOGCAT="01-01 00:00:00.000  1234  1234 I UnfuseZygisk: хуки libc в uid=10123: open=ok openat=ok mkdir=alias"
check "отчёт с ok" "libc_hooks=1" "$(eval "$LH")"

STUB_LOGCAT="01-01 00:00:00.000  1234  1234 I UnfuseZygisk: хуки libc в uid=10123: open=СБОЙ openat=нет mkdir=коротка"
check "отчёт без ok и alias" "libc_hooks=0" "$(eval "$LH")"

STUB_LOGCAT="01-01 00:00:00.000  1234  1234 I UnfuseZygisk: sdcardfs подключён: uid=10123"
check "чужая строка того же тега" "libc_hooks=0" "$(eval "$LH")"

# Kill switch: the module deliberately opts out of the hook.
STUB_MAPPED=1
STUB_LOGCAT=""
NO_HOOKS=$HERE/../module/status.sh   # any existing file instead of the marker
check "есть метка no_hooks" "libc_hooks=0" "$(eval "$LH")"

NO_HOOKS=/nonexistent/no_hooks
STUB_MAPPED=1
check "метки нет, отображён" "libc_hooks=1" "$(eval "$LH")"

# On the main path, mapping proves nothing.
all_sd
STUB_LOGCAT=""
all_sd
all_sd=1
check "основной путь, отображён, строк нет" "libc_hooks=0" "$(eval "$LH")"

echo
if [ "$fail" = 0 ]; then
    echo "всё сходится"
else
    echo "есть расхождения"
fi
exit "$fail"
