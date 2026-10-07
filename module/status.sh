#!/system/bin/sh
#
# status.sh — one sign in the description in module.prop: is vold's FUSE mount
# for emulated storage cut off.
#
# Zygisk Next does the same thing: the module list shows the shipped description
# with a status prefix, rewritten in place. There is no WebUI here, so that sign
# is the only place the module reports anything.
#
#   [✅ fuse-off] Direct internal storage for all apps — no FUSE, no scoped...
#
# The sign is the branch's whole point: this module works only if vold's FUSE
# mount for emulated storage is redirected into a bind of /data/media. That patch
# lives in vold's process memory, is applied at boot by post-fs-data.sh with
# tools/vold-fusefs, and can be lost on its own (vold restarted, patch failed to
# install). So the sign reads the patch state from the tool itself, which is the
# same question storage.sh step 4 asks — and unlike that one it is what the
# module list shows without opening a log.
#
# vold-fusefs --check: 0 patched, 1 no vold / none patched, 2 anchor unresolved,
# 3 not writable. Only 0 is a checkmark.
#
# Runs from service.sh, and can be run by hand:
#   su -c 'sh /data/adb/modules/unfuse_zygisk/status.sh'
# Exits 0 when FUSE is off, 1 when it is not.
#

# PATH is set explicitly: nothing here needs more than toybox, and the PATH a
# module stage inherits is not to be trusted.
PATH=/system/bin:/system/xbin
export PATH

MODDIR="${MODDIR:-${0%/*}}"
PROP="$MODDIR/module.prop"
BASE="$MODDIR/description.txt"

# The mark itself carries no brackets; they go on once, around the whole prefix,
# where the line is assembled below.
FUSEFS="$MODDIR/tools/vold-fusefs"
if [ -x "$FUSEFS" ] && "$FUSEFS" --check >/dev/null 2>&1; then
    MARK="✅ fuse-off"
    exit_status=0
else
    MARK="❌ fuse-off"
    exit_status=1
fi

# The base text lives in description.txt, not in the description line being
# rewritten below: that line is regenerated from scratch, so the base has to
# come from a file that is never overwritten.
base=$(sed -n '1p' "$BASE" 2>/dev/null)
[ -n "$base" ] || base="Unfuse Zygisk"

want="[$MARK] $base"
cur=$(sed -n 's/^description=//p' "$PROP" 2>/dev/null | tail -n 1)
cur_count=$(grep -c '^description=' "$PROP" 2>/dev/null || echo 0)

# The count matters as much as the text: a module.prop carrying two description
# lines would pass a text-only comparison and keep the stale second one forever,
# since every reader takes the first.
if [ "$cur" = "$want" ] && [ "$cur_count" = 1 ]; then
    echo "$want"
    exit $exit_status
fi

if [ ! -f "$PROP" ] || [ ! -w "$PROP" ]; then
    echo "$want"
    echo "status.sh: нет доступа к $PROP — описание не обновлено" >&2
    exit $exit_status
fi

# Written through a temp file in the same directory and moved into place, so a
# reader never sees a half-written module.prop — and so the permissions and
# owner set on the temp file are the ones that end up on module.prop (0644
# root:root, which is what set_perm left at install).
#
# Any further description lines are dropped, not left in place: a file that
# somehow carries two would otherwise keep a stale second copy forever, since
# every reader takes the first one and this script only ever writes the first.
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
    echo "status.sh: не удалось собрать $tmp" >&2
    exit $exit_status
fi

chmod 0644 "$tmp" 2>/dev/null
chown 0:0 "$tmp" 2>/dev/null
mv -f "$tmp" "$PROP" 2>/dev/null || {
    rm -f "$tmp" 2>/dev/null
    echo "$want"
    echo "status.sh: не удалось заменить $PROP" >&2
    exit $exit_status
}

echo "$want"
exit $exit_status
