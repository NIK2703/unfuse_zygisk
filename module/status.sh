#!/system/bin/sh
# status.sh — puts one sign into module.prop's description: whether vold's FUSE
# mount for emulated storage is cut off. vold-fusefs --check: 0 patched (the
# only checkmark), 1 no vold / none patched, 2 anchor unresolved, 3 not
# writable. One description= line; every reader takes the first.
# By hand: su -c 'sh /data/adb/modules/unfuse_zygisk/status.sh'

# PATH is set explicitly: the PATH a module stage inherits is not to be trusted.
PATH=/system/bin:/system/xbin
export PATH

MODDIR="${MODDIR:-${0%/*}}"
PROP="$MODDIR/module.prop"
BASE="$MODDIR/description.txt"

FUSEFS="$MODDIR/tools/vold-fusefs"
if [ -x "$FUSEFS" ] && "$FUSEFS" --check >/dev/null 2>&1; then
    MARK="✅"
    exit_status=0
else
    MARK="❌"
    exit_status=1
fi

base=$(sed -n '1p' "$BASE" 2>/dev/null)
[ -n "$base" ] || base="Unfuse Zygisk"

want="[$MARK] $base"
cur=$(sed -n 's/^description=//p' "$PROP" 2>/dev/null | tail -n 1)
cur_count=$(grep -c '^description=' "$PROP" 2>/dev/null || echo 0)

# A second description= line would pass a text-only check, so count it too.
if [ "$cur" = "$want" ] && [ "$cur_count" = 1 ]; then
    echo "$want"
    exit $exit_status
fi

if [ ! -f "$PROP" ] || [ ! -w "$PROP" ]; then
    echo "$want"
    exit $exit_status
fi

# Temp file in the same directory, then mv: no reader sees a half-written
# module.prop. Extra description lines are dropped, not kept.
tmp="$PROP.status.$$"
awk -v want="description=$want" '
    BEGIN { done = 0 }
    /^description=/ { if (!done) { print want; done = 1 } ; next }
    { print }
    END { if (!done) print want }
' "$PROP" > "$tmp" 2>/dev/null

if [ ! -s "$tmp" ]; then
    rm -f "$tmp" 2>/dev/null
    echo "$want"
    exit $exit_status
fi

chmod 0644 "$tmp" 2>/dev/null
chown 0:0 "$tmp" 2>/dev/null
mv -f "$tmp" "$PROP" 2>/dev/null || {
    rm -f "$tmp" 2>/dev/null
    echo "$want"
    exit $exit_status
}

echo "$want"
exit $exit_status
