#!/system/bin/sh
#
# status.sh — module state for the WebUI: one key=value line per check, exactly
# what webroot/index.html draws as checkmarks. Read-only; changes nothing.
#
# Manual run: su -c 'sh /data/adb/modules/unfuse_zygisk/status.sh'
#
# Keys:
#   kernel_sdcardfs  kernel has sdcardfs (otherwise the sdcardfs mode is off)
#   mode             what the config says (auto|sdcardfs|acl|fuse)
#   effective        path actually in use right now
#   mount_<point>    /mnt/runtime/<point>/emulated is an sdcardfs mount
#   mount_opts       all four points carry the requested mask and gid
#   label_media      /data/media root relabelled to media_rw_data_file
#   acl_access       access ACL of /data/media/0 has an entry for 9997
#   acl_default      default ACL of /data/media/0 has an entry for 9997
#   vold_patched     setxattr defused in the running vold
#   fuse_off         vold's FUSE mount redirected to a bind (mode=fuse only)
#   libc_hooks       libc entry patch applied inside app processes
#

# PATH is set explicitly: the page runs this through ksu.exec, where the
# inherited PATH is not to be trusted. /system/bin covers toybox, which is all
# this needs.
PATH=/system/bin:/system/xbin
export PATH

MODDIR="${MODDIR:-${0%/*}}"
CONF="$MODDIR/unfuse_zygisk.conf"
POINTS_FILE="$MODDIR/storage.sh"
# Same switch the module itself checks (src/unfuse_zygisk.cpp, kNoHooksFlag).
NO_HOOKS=/data/adb/unfuse_zygisk.no_hooks

kv() { echo "$1=$2"; }

# --- expectations come from storage.sh, not from a copy here -------------------
# The points and the options requested at each live in storage.sh as the single
# line SDCARDFS_POINTS. A second copy here would be a second source of truth: an
# edit to storage.sh would go unnoticed and the checkmarks would drift.
POINTS=$(sed -n 's/^SDCARDFS_POINTS=//p' "$POINTS_FILE" 2>/dev/null | tr -d '"')
[ -n "$POINTS" ] || POINTS="default:6:1015 read:23:9997 write:7:9997 full:7:9997"

SDCARDFS_FS=sdcardfs

# FS type under a point: the last /proc/mounts entry wins, since mounts stack.
fs_type() {
    t=""
    while read -r _dev mp ty _rest; do
        [ "$mp" = "$1" ] && t="$ty"
    done < /proc/mounts
    echo "$t"
}

# Mount options of a point.
mount_opts() {
    o=""
    while read -r _dev mp _ty opts _rest; do
        [ "$mp" = "$1" ] && o="$opts"
    done < /proc/mounts
    echo "$o"
}

# Is $2 among the comma-separated options $1; the commas keep "gid=1015" from
# matching "fsgid=1015".
has_opt() {
    case ",$1," in
        *",$2,"*) return 0 ;;
        *)        return 1 ;;
    esac
}

# Is the module mapped into any process. Weak evidence on its own (see the libc
# note); a function so a test can stub it.
module_mapped() {
    grep -lqs "unfuse_zygisk" /proc/*/maps 2>/dev/null
}

# --- kernel -------------------------------------------------------------------
if grep -qw sdcardfs /proc/filesystems 2>/dev/null; then
    kv kernel_sdcardfs 1
else
    kv kernel_sdcardfs 0
fi

# --- mode from the config -----------------------------------------------------
# acl is the current name of the third mode, raw the old one.
mode=$(sed -n 's/^[[:space:]]*path[[:space:]]*=[[:space:]]*//p' "$CONF" 2>/dev/null \
    | sed 's/[[:space:]]*#.*$//' | sed 's/[[:space:]]*$//' | tail -n 1)
case "$mode" in
    auto|sdcardfs|fuse) ;;
    acl|raw)            mode=acl ;;
    *)                  mode=auto ;;
esac
kv mode "$mode"

# --- which path is actually in use --------------------------------------------
# Not the config but the mounts: sdcardfs under all four points means the main
# path, anything else the fallback. The checkmarks describe what is, not what was
# asked for.
all_sd=1
for spec in $POINTS; do
    [ "$(fs_type "/mnt/runtime/${spec%%:*}/emulated")" = "$SDCARDFS_FS" ] || all_sd=0
done

if [ "$all_sd" = 1 ]; then
    kv effective sdcardfs
else
    kv effective acl
fi

# --- main path: one checkmark per point ---------------------------------------
for spec in $POINTS; do
    name="${spec%%:*}"
    if [ "$(fs_type "/mnt/runtime/$name/emulated")" = "$SDCARDFS_FS" ]; then
        kv "mount_$name" 1
    else
        kv "mount_$name" 0
    fi
done

# --- main path: mount options -------------------------------------------------
# Same point as storage.sh step 2a: a mount(2) return code does not mean the
# kernel applied the options. The checkmark claims "the options carry the mask
# and gid we asked for", so it only fires when there is something to stand on:
# all four points must be our sdcardfs mounts (a missing point has empty options,
# and "no options" is indistinguishable from "no point"), and mask= and gid= must
# each show up at least once, since the checkmark claims both.
#
opts_ok=1
seen_mask=0
seen_gid=0
for spec in $POINTS; do
    name="${spec%%:*}"
    rest="${spec#*:}"
    want_mask="${rest%%:*}"
    want_gid="${rest##*:}"
    p="/mnt/runtime/$name/emulated"

    if [ "$(fs_type "$p")" != "$SDCARDFS_FS" ]; then
        opts_ok=0
        continue
    fi

    o=$(mount_opts "$p")
    case ",$o," in
        *,mask=*) seen_mask=1; has_opt "$o" "mask=$want_mask" || opts_ok=0 ;;
    esac
    case ",$o," in
        *,gid=*)  seen_gid=1;  has_opt "$o" "gid=$want_gid"   || opts_ok=0 ;;
    esac
done
if [ "$seen_mask" != 1 ] || [ "$seen_gid" != 1 ]; then
    opts_ok=0
fi
kv mount_opts "$opts_ok"

# --- label of the /data/media root --------------------------------------------
# Without media_rw_data_file the /storage/emulated root returns EACCES: appdomain
# has one search and no getattr on media_userdir_file.
cur=$(ls -Zd /data/media 2>/dev/null | awk '{print $1}')
case "$cur" in
    *:media_rw_data_file:*) kv label_media 1 ;;
    *)                      kv label_media 0 ;;
esac

# --- ACL on the raw tree ------------------------------------------------------
# storage-fix --check prints "access=..." and "default=..." separately, so the
# checkmarks are separate too: the default ACL is what new files inherit and the
# one vold used to overwrite.
FIX="$MODDIR/tools/storage-fix"
if [ -x "$FIX" ]; then
    out=$("$FIX" --check /data/media/0 2>/dev/null)
    case "$out" in
        *"access=нет 9997"*) kv acl_access 0 ;;
        *"access="*)         kv acl_access 1 ;;
        *)                   kv acl_access 0 ;;
    esac
    case "$out" in
        *"default=нет 9997"*) kv acl_default 0 ;;
        *"default="*)         kv acl_default 1 ;;
        *)                    kv acl_default 0 ;;
    esac
else
    kv acl_access 0
    kv acl_default 0
fi

# --- vold patch ---------------------------------------------------------------
# vold-noacl --check: 0 patched, 1 trampoline intact (or no vold), 2 unparsable,
# 3 not writable. Only 0 counts.
NOACL="$MODDIR/tools/vold-noacl"
if [ -x "$NOACL" ] && "$NOACL" --check >/dev/null 2>&1; then
    kv vold_patched 1
else
    kv vold_patched 0
fi

# --- FUSE-off patch -----------------------------------------------------------
# vold-fusefs --check exits 0 only when the trampoline is redirected; 1 means no
# vold (or none patched), 2 that the anchor did not resolve. Only 0 counts.
#
# Reported always, not only in mode=fuse: the key answers "is vold still mounting
# FUSE", which is a fact about the machine, and the page can decide what to do
# with it. In the other modes 0 is expected.
FUSEFS="$MODDIR/tools/vold-fusefs"
if [ -x "$FUSEFS" ] && "$FUSEFS" --check >/dev/null 2>&1; then
    kv fuse_off 1
else
    kv fuse_off 0
fi

# --- libc entry patch ---------------------------------------------------------
# The patch lives in the app's own COW memory, invisible from outside — not in
# /proc, not on disk — so we go by what the module logged. postAppSpecialize
# prints "хуки libc в uid=<N>: <target>=<state>..." when it installed it, and
# complains alongside if no target took. Success is ok or alias for at least one
# target: the same tally the module uses to call the install failed.
#
# The module prints it once per boot — the first app that gets raw storage — and
# stays quiet afterwards, so the tag carries one sample, not one line per launch.
#
#   line with ok/alias     — applied, direct evidence;
#   line without ok/alias  — nothing took (a bare "хуки libc" match used to light
#                            the checkmark even on the total-failure line);
#   no line at all         — the first launch was on the main path, or logcat
#                            rolled over; fall back to the module being mapped
#                            into an app process. That means nothing on the main
#                            path (no patch needed there) and nothing when
#                            /data/adb/unfuse_zygisk.no_hooks is present.
#
hooks=0
if [ ! -e "$NO_HOOKS" ]; then
    said=$(logcat -d -s UnfuseZygisk 2>/dev/null | grep "хуки libc в uid=")
    if [ -n "$said" ]; then
        case "$said" in
            *"=ok"*|*"=alias"*) hooks=1 ;;
        esac
    elif [ "$all_sd" != 1 ] && module_mapped; then
        hooks=1
    fi
fi
kv libc_hooks "$hooks"

exit 0
