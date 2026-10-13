#!/system/bin/sh
# service.sh — late boot, after vold mounted the storages: repeat the
# preparation (idempotent) and make the one decision post-fs-data cannot.
#
# Модуль ничего не пишет: ни журнала, ни logcat — отчёт только кодом возврата.

MODDIR=${MODDIR:-${0%/*}}

# Both vold patches live in vold's memory and vold may not have existed at
# post-fs-data, so repeat them. vold-fusefs first: whether it took is the premise
# the decision below is made on.
FUSEFS="$MODDIR/tools/vold-fusefs"
if [ -x "$FUSEFS" ]; then
    "$FUSEFS"
fi

# The premise, checked once, here — the only point in the boot where it is
# answerable, because vold has mounted the volumes by now.
#
# With the bind in place /storage/emulated/0 is the raw f2fs tree (0xf2f52010).
# If it is still fuse (0x65735546) the module is half-applied, and in that state
# tools/vold-noacl does harm and no good: it suppresses vold's own per-package
# default ACL (the single setxattr behind SetDefaultAcl, vold-11/Utils.cpp:398)
# while the module's replacement for it is not what is serving /storage anyway.
# Skipping it lets vold write, which restores AOSP's guarantee for the app's own
# Android/data/<pkg>.
#
# Форма хранилища — не единственный вход, и сама по себе она отвечает не всегда.
# Замер 2026-10-09 (redroid A11, ветка no-sdcardfs): в момент service.sh
# /storage/emulated/0 = 1021994, то есть 0x01021994 = tmpfs — монтирования ещё
# нет вовсе (бинды /storage делает триггер init.rc на zygote-start, позже
# service). «Не fuse» тогда означает не «bind встал», а «ещё не спрашивали».
# Поэтому решение опирается на прямой признак: `vold-fusefs --check` = 0 ровно
# тогда, когда патч лежит в памяти vold (tools/vold-fusefs.c:239). Именно этот
# патч и есть модель модуля — без него MountUserFuse() не закончится биндом
# сырого /data/media, и глушить ACL vold было бы вредно. Форма хранилища
# остаётся вторым входом: если монтирование уже состоялось и оно fuse — патч
# есть, а бинда нет, и это тоже «не ставим».
#
# This is a one-shot decision at a known point in the boot, not a watcher: the
# project's rule against guards is about processes that keep re-deciding, and
# this one never runs a second time.
NOACL="$MODDIR/tools/vold-noacl"
FS_TYPE=$(stat -f -c %t /storage/emulated/0 2>/dev/null)
"$FUSEFS" --check >/dev/null 2>&1
FUSEFS_OK=$?
if [ -x "$NOACL" ] && [ "$FUSEFS_OK" = "0" ] && [ "$FS_TYPE" != "65735546" ]; then
    "$NOACL"
fi

# vold may restore the media_userdir_file label while preparing /data/media.
# Last, so that the tree the module shapes is the last word: it runs after vold
# has done its own CE work and after both patches.
sh "$MODDIR/storage.sh"

# --- vold is NOT done with the emulated volume at this point ----------------
#
# Reported end state (crDroid 7 / A11, sdcardfs in the kernel,
# CONFIG_EXT4_FS_POSIX_ACL off): the raw bind itself is up — /storage/emulated/0
# is ext4 over /data/media — yet /storage/emulated/0/Android and
# /storage/emulated/0/Android/data come back with their AOSP rights
# (media_rw:media_rw 02771 and media_rw:ext_data_rw 02771) and every app probe on
# .../Android/data gets EACCES: traverse, no listing. Two independent things
# happen after this script has already run, and both are vold's:
#
#   1. EmulatedVolume::mount() (model/EmulatedVolume.cpp:353-403) runs when the
#      volume is mounted — at user start, after the late_start stage this script
#      belongs to. Inside it:
#
#      * PrepareAndroidDirs() (Utils.cpp:1727) fs_prepare_dir()s Android and
#        Android/{data,obb,media}: chown to media_rw/ext_data_rw and chmod 02771.
#        On a kernel with POSIX ACL the named 9997 entry survives a chmod; on a
#        kernel without one the grant rides gid 9997 (grant_by_group,
#        tools/storage-fix.c) and a chown destroys it. Fix F's window covers the
#        volume root; this is the same race one level deeper, and its writer is
#        the next bullet.
#      * mountFuseBindMounts() (EmulatedVolume.cpp:112-136) then bind-mounts
#        /mnt/runtime/default/<label>/<user>/Android/{data,obb} over
#        /mnt/user/<user>/emulated/<user>/Android/{data,obb}. Where vold raised
#        sdcardfs that source is the sdcardfs DEFAULT view — AID_SDCARD_RW 1015,
#        mask 0006 → 0771 — so an app in group 9997 has other = --x there. The
#        vold-fusefs header calls these binds "self-binds", which is true only
#        while mUseSdcardFs is false; where it is true they hide the raw tree the
#        module put underneath. mUseSdcardFs cannot be turned off from a module
#        script: it is cached in the EmulatedVolume constructor when vold starts
#        (init.rc:453 `start vold` in early-fs, which precedes post-fs-data), so
#        external_storage.sdcardfs.enabled=0 lands after the read. The same
#        cached value is why the SD card had to be bound from the raw
#        /mnt/media_rw instead of from the views.
#   2. fscrypt_prepare_user_storage() (FsCrypt.cpp:1021) chowns the volume root
#      later still — that one Fix F's window already covers.
#
# Hence the order below: wait until vold is observably through with the volume,
# drop the stale view off the app's Android/{data,obb}, then re-award. One-shot
# at this point of the boot and idempotent, like everything else in this script:
# the wait is bounded and exits at its deadline, no second process is started.

# The signal that vold is through with the emulated volume:
# /mnt/pass_through/0/emulated is bound by MountUserFuse()'s tail
# (Utils.cpp:1568-1577), which runs after PrepareAndroidDirs(); the app-view
# bind from mountFuseBindMounts() follows one daemon-wait later. When
# mAppDataIsolationEnabled (persist.sys.isolated_storage, EmulatedVolume.cpp:53,
# default false) is true that app-view bind is never made at all, so the grace
# below covers both ROM shapes.
EMU_VIEW=/mnt/user/0/emulated
PASS_THROUGH=/mnt/pass_through/0/emulated
APP_DATA=/mnt/user/0/emulated/0/Android/data
APP_OBB=/mnt/user/0/emulated/0/Android/obb

# Last /proc/1/mounts entry for a mountpoint wins, since mounts stack — a grep
# would also see what lies underneath (/proc/1/mounts, not the caller's own
# namespace: the mounts that matter are vold's, made in the init ns).
mounted_type() {
    _t=""
    while read -r _dev mp ty _rest; do
        [ "$mp" = "$1" ] && _t="$ty"
    done < /proc/1/mounts 2>/dev/null
    echo "$_t"
}

# 0 = the signal never showed (bounded wait expired); otherwise the epoch second
# it was first seen. The award loop uses the age of this moment, not the wait.
T_SIGNAL=0
WAIT_DEADLINE=$(( $(date +%s) + 120 ))
while :; do
    if [ -n "$(mounted_type "$PASS_THROUGH")" ]; then
        T_SIGNAL=$(date +%s)
        break
    fi
    [ "$(date +%s)" -ge "$WAIT_DEADLINE" ] && break
    sleep 1
done

# mountFuseBindMounts()'s tail: the app-view bind, if this ROM makes one. It
# trails the signal by the FUSE-daemon wait inside doMount()
# (EmulatedVolume.cpp:390-397) — milliseconds on a healthy boot.
GRACE_DEADLINE=$(( $(date +%s) + 10 ))
while [ -z "$(mounted_type "$APP_DATA")" ] && [ -z "$(mounted_type "$APP_OBB")" ]; do
    [ "$(date +%s)" -ge "$GRACE_DEADLINE" ] && break
    sleep 1
done

# Take the stale view off the app's Android/{data,obb} — but only when the raw
# bind is really underneath it. The premise is the same one the vold-noacl
# decision above uses: with vold-fusefs in memory the topmost filesystem on
# /mnt/user/0/emulated is the raw tree, and dropping the view exposes that tree,
# with the grant storage.sh makes on it. Without the bind the parent is the FUSE
# mount itself, and exposing the daemon's view of Android/data instead would
# move things backwards — the half-applied state is not what this stage fixes.
# MNT_DETACH (umount -l): the view is a shared mount apps are standing on, and
# a plain unmount would return EBUSY. Returns 0 when it dropped something.
drop_stale_view() {
    [ "$FUSEFS_OK" = "0" ] || return 1
    [ "$(mounted_type "$EMU_VIEW")" != "fuse" ] || return 1
    _dropped=0
    for p in "$APP_DATA" "$APP_OBB"; do
        if [ -n "$(mounted_type "$p")" ]; then
            if umount -l "$p" 2>/dev/null || umount "$p" 2>/dev/null; then
                _dropped=1
            fi
        fi
    done
    [ "$_dropped" = "1" ]
}

drop_stale_view || :

# storage.sh once more, now as the last word on the raw Android* levels: the
# signal above proves PrepareAndroidDirs() has already run, so this grant is not
# followed by a vold chown. The writers that remain — per-package dirs at
# install time, and the volume root Fix F's window re-checks — are elsewhere.
sh "$MODDIR/storage.sh"

# A bind that landed during the grace (the daemon wait is not a promise) leaves
# the grant masked; one more check costs a couple of /proc reads.
if drop_stale_view; then
    sh "$MODDIR/storage.sh"
fi

# --- The award has to outlive vold's own writers ---
#
# On a kernel without POSIX ACL (the x0 device) the grant rides the group: gid
# 9997 and S_ISGID, set by storage-fix (grant_by_group). Unlike a named ACL
# entry, that grant is not protected from a plain chown/chmod — and vold does
# exactly that, twice, well after this script has run:
#
#   * fscrypt_prepare_user_storage() (FsCrypt.cpp:921, CE branch :1021)
#     -> fs_prepare_dir("/data/media/<u>", 02770, AID_MEDIA_RW, AID_MEDIA_RW)
#     resets owner and mode of the volume root, dropping gid 9997 and S_ISGID.
#     Reachable from StorageManagerService via binder, and on the reported
#     device it fired ~47 s AFTER service.sh (16:18:22 -> 16:19:09).
#   * EmulatedVolume::mount() (EmulatedVolume.cpp:436) -> PrepareAndroidDirs()
#     (Utils.cpp:1727) -> fs_prepare_dir on Android, Android/{data,obb,media}
#     with mode 02771 and AID_MEDIA_RW — the four Android* levels go the same
#     way, at the moment the volume is mounted (also after service.sh).
#
# tools/vold-noacl cannot stop either: it neutralises the one setxattr behind
# SetDefaultAcl (the ACL write), not chown/chmod — and on a kernel without POSIX
# ACL that write was a no-op all the same. Intercepting chown/chmod inside vold
# would close the cause, but it means a third hand-emitted arm64 handler living
# in vold's address space, which the project's own rule (emitted code must be
# proven by execution on aarch64 — tools/vold-fusefs.c, build_stub_patch) puts
# out of reach for exactly this class of change.
#
# So the award is repeated instead, over a bounded window that spans the point
# where vold is done. The window is deliberately not tied to a single property:
# the observed writer fired ~47 s after service.sh, and whether that lands
# before or after sys.boot_completed differs by device and by vold build, so
# the correctness below does not rest on that ordering. The property is only an
# optimisation — wait for the boot to settle, then re-award until the grant is
# actually in place or the deadline passes. Waiting is bounded and one-shot:
# this is the same single decision point the rest of the script uses, not a
# watcher — the loop exits unconditionally at the deadline and starts no second
# process.
#
# A device with POSIX ACL is unaffected: there chmod leaves named entries alone
# and --check already passes, so the loop body never runs. Nothing here reads
# or writes a journal (the module reports by exit code only).
FIX="$MODDIR/tools/storage-fix"

# Wait for the boot to settle, bounded. Purely an optimisation: the settle loop
# below is what makes the award hold. If boot_completed never arrives, the
# deadline breaks here and the loop still runs.
BOOT_DEADLINE=$(( $(date +%s) + 120 ))
while [ "$(getprop sys.boot_completed)" != "1" ]; do
    [ "$(date +%s)" -ge "$BOOT_DEADLINE" ] && break
    sleep 1
done

# The award names two targets, and the vold writers do not hit the same one:
# fscrypt_prepare_user_storage resets the volume root, while the Android* levels
# are re-awarded by the corrective stage above (its signal proves
# PrepareAndroidDirs has already run). --check covers both in one call and
# reports the whole grant (gid 9997, group bits, S_ISGID on a dir), so a zero
# here means the kernel will really let an app in. The Android tree is checked
# only when it exists — a volume that never expanded has no such level, and
# asking about a missing path would only spin the loop.
award_ok() {
    [ -x "$FIX" ] || return 1
    if [ -d /data/media/0/Android ]; then
        "$FIX" --check /data/media/0 /data/media/0/Android >/dev/null 2>&1
    else
        "$FIX" --check /data/media/0 >/dev/null 2>&1
    fi
}

# The settle window. It has to reach past the writer that is left, which on the
# reported device was ~47 s after service.sh; 90 s covers that with margin, and
# a vold restart inside the window is covered too (the re-award simply repeats).
# The re-award is the full storage.sh pass — a recursive walk of /data/media — so
# it is spaced out rather than run every tick; the check in between is cheap.
# Unconditional exit at the deadline: the loop is finite and does not outlive the
# boot stage it belongs to.
#
# The break is deliberately gated: an award that merely survived so far is not
# proof the writers are done, and this loop used to exit on the first green
# check — which on the reported device was before the volume had even mounted,
# so nothing re-awarded the Android* levels afterwards. It now breaks only once
# the boot has settled AND the volume mount is a moment that is at least 15 s in
# the past, so the late root writer lands inside the window instead of behind
# the break. If boot never settles, or the volume never mounts, the deadline
# breaks the loop anyway.
AWARD_DEADLINE=$(( $(date +%s) + 90 ))
last_pass=0
while :; do
    now=$(date +%s)
    if [ "$(getprop sys.boot_completed)" = "1" ] &&
       [ "$T_SIGNAL" -gt 0 ] && [ $(( now - T_SIGNAL )) -ge 15 ]; then
        award_ok && break
    fi
    [ "$now" -ge "$AWARD_DEADLINE" ] && break
    if [ $(( now - last_pass )) -ge 5 ]; then
        sh "$MODDIR/storage.sh"
        last_pass=$now
    fi
    sleep 1
done

exit 0
