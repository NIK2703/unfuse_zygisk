#!/system/bin/sh
# sdcardfs DAC model as a REAL app process (Zygote uid/gid/groups: 9997 AID_EVERYBODY,
# inet 3003...). Masks decimal, as sdcard.cpp prints: default 0006→"6", read 0027→"23",
# write/full 0007→"7". All in /data/local/tmp, removed afterwards.

SRC=/data/media
BASE=/data/local/tmp/sdtest
RUNAS=/data/local/tmp/runas

# ai.qwenlm.chat.android is a real installed package
APPUID=10465
APPGIDS="10465,20465,9997,3003"

echo "===== 0. ПОДГОТОВКА ====="
chmod 755 "$RUNAS"
"$RUNAS" 10465 10465 "10465,20465,9997,3003" id 2>&1
echo "--- контроль: хелпер работает? ---"
"$RUNAS" 0 0 "0" id 2>&1

rm -rf "$BASE"; mkdir -p "$BASE"; chmod 755 "$BASE"; chmod 755 /data/local/tmp

OWNPKG=ai.qwenlm.chat.android
OTHERPKG=$(ls /data/media/0/Android/data 2>/dev/null | grep -v "^$OWNPKG$" | grep -v '^\.' | head -1)
echo "OWNPKG=$OWNPKG  OTHERPKG=$OTHERPKG"

case_run() {
    name="$1"; mask="$2"; gid="$3"
    dst="$BASE/$name"
    mkdir -p "$dst"; chmod 755 "$dst"

    echo
    echo "############ $name   mask=$mask(dec)  gid=$gid ############"
    mount -t sdcardfs \
        -o "fsuid=1023,fsgid=1023,multiuser,derive_gid,default_normal,unshared_obb,mask=$mask,userid=0,gid=$gid" \
        "$SRC" "$dst" || { echo "MOUNT FAILED"; return; }

    echo "--- режимы (как их видит stat) ---"
    for p in "" /0 /0/Download /0/Android /0/Android/data "/0/Android/data/$OWNPKG" "/0/Android/data/$OTHERPKG" /0/Android/obb; do
        stat -c '  %A %a uid=%u gid=%g %n' "$dst$p" 2>&1
    done

    echo "--- ПРИЛОЖЕНИЕ uid=$APPUID gids=$APPGIDS ---"
    tapp() {
        desc="$1"; shift
        if "$RUNAS" "$APPUID" "$APPUID" "$APPGIDS" "$@" >/dev/null 2>&1; then
            echo "  OK   $desc"
        else
            echo "  DENY $desc"
        fi
    }
    tapp "list /"                        ls "$dst/0"
    tapp "list Download"                 ls "$dst/0/Download"
    tapp "READ файл в Download"          head -c 16 "$dst/0/Download"/*.zip
    tapp "WRITE в Download"              touch "$dst/0/Download/.probe_$name"
    tapp "list Android/data"             ls "$dst/0/Android/data"
    tapp "list Android/obb"              ls "$dst/0/Android/obb"
    tapp "СВОЙ Android/data ($OWNPKG)"   ls "$dst/0/Android/data/$OWNPKG"
    if [ -n "$OTHERPKG" ]; then
        tapp "ЧУЖОЙ Android/data ($OTHERPKG)" ls "$dst/0/Android/data/$OTHERPKG"
    fi

    rm -f "$dst/0/Download/.probe_$name" 2>/dev/null

    echo "--- SHELL uid=2000 gids=2000,9997,3003,1015 ---"
    tsh() {
        desc="$1"; shift
        if "$RUNAS" 2000 2000 "2000,9997,3003,1015" "$@" >/dev/null 2>&1; then
            echo "  OK   $desc"
        else
            echo "  DENY $desc"
        fi
    }
    tsh "list /"              ls "$dst/0"
    tsh "list Download"       ls "$dst/0/Download"
    tsh "list Android/data"   ls "$dst/0/Android/data"

    umount "$dst" && echo "  [unmounted]" || echo "  [UMOUNT FAILED]"
}

case_run default 6  1015
case_run read    23 9997
case_run write   7  9997
case_run full    7  9997

echo
echo "===== УБОРКА ====="
rm -rf "$BASE"
echo "===== КОНЕЦ ====="
