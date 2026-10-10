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
