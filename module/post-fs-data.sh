#!/system/bin/sh
#
# post-fs-data.sh — prepare storage before Zygote starts.
#
# The Zygisk module mounts nothing — it only clears mount_storage_dirs at
# specialisation. Storage reaches the apps through vold, so by the time the first
# app launches these must already exist:
#
#   * the media_rw_data_file label on the /data/media root;
#   * the ACL for group 9997 on the raw /data/media tree;
#   * vold's FUSE mount for emulated storage redirected away, so the apps reach
#     the raw tree rather than FUSE;
#   * the module's state directory, labelled so the module's own domain may
#     write to it.
#
# storage.sh does the first two, the vold patch below does the third, and the
# first section here does the fourth.
#
# The third has to happen before vold prepares the user's storage rather than
# after: tools/vold-fusefs is pointed at vold to redirect its mount()
# trampoline, so the FUSE mount for emulated storage is turned into a bind of the
# raw tree instead of being made at all.
#

MODDIR=${MODDIR:-${0%/*}}
LOG=/data/adb/unfuse_zygisk.log

# --- state directory for the native module ------------------------------------
#
# The Zygisk module keeps two files: `once`, claimed by the first app that gets
# storage so the tag carries one sample per boot, and `no_hooks`, created by hand
# to switch the libc patching off.
#
# They cannot live in /data/adb itself. The module runs in the zygote domain —
# preAppSpecialize is called before the process specialises — and the only thing
# the policy gives that domain over /data/adb is traversal:
#
#   allow zygote adb_data_file dir search          (zygisksu/sepolicy.rule)
#
# search is what the path walk to the module's own .so needs, and it is all
# there is. open(O_CREAT) there needs add_name and write on the directory, so it
# fails, and the failure is dontaudit'ed: nothing in logcat, nothing anywhere.
# Measured on the device by running permissive for four seconds — the only way to
# see an unaudited denial — the marker appeared and the sample was written the
# moment SELinux stopped blocking, with no avc line either way. Full account in
# the header of src/unfuse_zygisk.cpp.
#
# So the two files get a directory of their own, labelled with the one type the
# policy hands out to every domain:
#
#   allow * magisk_file dir *      allow * magisk_file file *
#
# and a file created inside such a directory inherits that label — verified on
# the device, where touch and mkdir inside it both came out magisk_file. That is
# what lets the module create and write its state from the zygote domain without
# a policy rule of its own.
#
# mkdir, not rm -rf: no_hooks is the user's file and has to survive a reboot. The
# label is re-applied every boot because nothing guarantees it survived the last
# one. Not repeated in service.sh either, unlike the vold patch: that one is
# repeated because vold may not exist yet at this stage, and this has no such
# dependency — if the directory is not there now, it never will be.
#
STATE=/data/adb/unfuse_zygisk.state
mkdir -p "$STATE" 2>/dev/null
chcon u:object_r:magisk_file:s0 "$STATE" 2>/dev/null

state_label=$(ls -Zd "$STATE" 2>/dev/null | cut -d' ' -f1)
if [ ! -d "$STATE" ]; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: нет $STATE —" \
        "модуль не сможет создать маркер и не напишет сэмпл в лог" >>"$LOG"
elif [ "$state_label" != "u:object_r:magisk_file:s0" ]; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: у $STATE метка" \
        "'$state_label', а не magisk_file — писать в неё модуль не сможет" >>"$LOG"
fi

# The marker has to be gone before Zygote starts, otherwise the tag stays silent
# for the whole boot after the first one. Here, not in service.sh: that runs after
# apps are already launching.
rm -f "$STATE/once" 2>/dev/null
if [ -e "$STATE/once" ]; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: маркер прошлой загрузки" \
        "не снялся — в этой загрузке сэмпла не будет" >>"$LOG"
fi

sh "$MODDIR/storage.sh" post-fs-data

# --- cut FUSE off in vold ----------------------------------------------------
#
# The deepest thing this branch does: instead of living alongside the FUSE mount
# and fighting it for permissions (which is what the ACL work in storage.sh
# does), vold's own mount() trampoline is redirected so that MountUserFuse()
# ends with a bind of /data/media on top of the FUSE mount it asked for. The
# FUSE superblock still exists — the fd it hands to MediaProvider is part of
# its contract, and a volume whose daemon does not start is reported
# "unmountable" — but nothing reaches it, because the bind is the topmost mount
# at the path.
#
# The same run redirects vold's umount2() trampoline too. That is not a
# separate feature: vold's teardown removes exactly one mount per path, so
# without it UnmountUserFuse() would take our bind off and leave the FUSE mount
# behind, and every later mount of the volume would fail with ENOTCONN. The two
# hooks go in together or not at all.
#
# The patch lives in process memory, so it is re-applied every boot; it is
# idempotent — on an already patched vold the tool confirms the patch instead of
# writing. Rationale for the patch itself: see the header of tools/vold-fusefs.c.
#
FUSEFS="$MODDIR/tools/vold-fusefs"
if [ ! -x "$FUSEFS" ]; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: нет $FUSEFS —" \
        "vold смонтирует FUSE, прямого доступа не будет" >>"$LOG"
elif "$FUSEFS" --wait 3 >>"$LOG" 2>&1; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: FUSE отключён в vold" >>"$LOG"
else
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] post-fs-data: vold-fusefs вернул $? —" \
        "FUSE останется смонтированным" >>"$LOG"
fi

# --- stop vold from writing the default ACL for group 1023 --------------------
#
# vold prepares the user's CE storage later than this stage: here it only does
# DE (vold-16/FsCrypt.cpp:657) and leaves CE to the framework — "the framework
# will prepare the user's CE storage later, once their CE key is installed".
# So we get in before its SetDefaultAcl(media_ce_path, ...) call.
#
# Ordered after the FUSE patch on purpose: both patch vold's memory, and the FUSE
# redirection changes which mount() calls happen at all. Applying them in this
# order means the ACL pass has already been neutralised regardless.
#
# The patch lives in process memory, so it must be re-applied after every
# reboot: here and, just in case, in service.sh. Both steps are idempotent — on
# a fresh vold the trampoline is intact, on an already patched one the tool just
# confirms the patch is in place.
#
# Rationale for the patch itself: see the header of tools/vold-noacl.c.
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
