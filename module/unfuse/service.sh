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

# The award names two targets, and the two vold writers do not hit the same
# one: fscrypt_prepare_user_storage resets the volume root, PrepareAndroidDirs
# resets the Android* levels. --check covers both in one call and reports the
# whole grant (gid 9997, group bits, S_ISGID on a dir), so a zero here means
# the kernel will really let an app in. The Android tree is checked only when
# it exists — a volume that never expanded has no such level, and asking about
# a missing path would only spin the loop.
award_ok() {
    [ -x "$FIX" ] || return 1
    if [ -d /data/media/0/Android ]; then
        "$FIX" --check /data/media/0 /data/media/0/Android >/dev/null 2>&1
    else
        "$FIX" --check /data/media/0 >/dev/null 2>&1
    fi
}

# The settle window. It has to reach past the later of the two writers, which
# on the reported device was ~47 s after service.sh; 90 s past the wait covers
# that with margin, and a vold restart inside the window is covered too (the
# re-award simply repeats). The re-award is the full storage.sh pass — a
# recursive walk of /data/media — so it is spaced out rather than run every
# tick; the check in between is cheap. Unconditional exit at the deadline: the
# loop is finite and does not outlive the boot stage it belongs to.
AWARD_DEADLINE=$(( $(date +%s) + 90 ))
last_pass=0
while :; do
    award_ok && break
    now=$(date +%s)
    [ "$now" -ge "$AWARD_DEADLINE" ] && break
    if [ $(( now - last_pass )) -ge 5 ]; then
        sh "$MODDIR/storage.sh"
        last_pass=$now
    fi
    sleep 1
done

exit 0
