#!/system/bin/sh
# status.sh — one sign in module.prop's description: is vold's FUSE mount for
# emulated storage cut off. Checkmark only while that mount stays redirected
# into a bind of /data/media; the patch lives in vold's process memory
# (post-fs-data.sh, tools/vold-fusefs) and dies with a vold restart or a failed
# patch, so the state is read from the tool. vold-fusefs --check: 0 patched,
# 1 no vold / none patched, 2 anchor unresolved, 3 not writable; only 0 is a
# checkmark (exit 0 = off, 1 = on); one description= line, every reader takes
# the first. By hand: su -c 'sh /data/adb/modules/unfuse_zygisk/status.sh'

# PATH is set explicitly: the PATH a module stage inherits is not to be trusted.
PATH=/system/bin:/system/xbin
export PATH

MODDIR="${MODDIR:-${0%/*}}"
PROP="$MODDIR/module.prop"
BASE="$MODDIR/description.txt"

# The mark carries no brackets; they go on around the whole prefix below.
FUSEFS="$MODDIR/tools/vold-fusefs"
if [ -x "$FUSEFS" ] && "$FUSEFS" --check >/dev/null 2>&1; then
    MARK="✅"
    exit_status=0
else
    MARK="❌"
    exit_status=1
fi

# Base text is in description.txt; the description line is rebuilt from scratch.
base=$(sed -n '1p' "$BASE" 2>/dev/null)
[ -n "$base" ] || base="Unfuse Zygisk"

want="[$MARK] $base"
cur=$(sed -n 's/^description=//p' "$PROP" 2>/dev/null | tail -n 1)
cur_count=$(grep -c '^description=' "$PROP" 2>/dev/null || echo 0)

# The count matters as much as the text: a second description= line would pass
# the text comparison and survive.
if [ "$cur" = "$want" ] && [ "$cur_count" = 1 ]; then
    echo "$want"
    exit $exit_status
fi

if [ ! -f "$PROP" ] || [ ! -w "$PROP" ]; then
    echo "$want"
    exit $exit_status
fi

# Written through a temp file in the same directory and moved into place, so a
# reader never sees a half-written module.prop, and 0644 root:root (as set_perm
# left at install) carries over from the temp file. Extra description lines are
# dropped, not kept.
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
