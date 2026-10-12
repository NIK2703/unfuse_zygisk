# shellcheck shell=sh
#
# customize.sh — called by the Magisk/KernelSU installer while unpacking the
# module (сборка unfuse-sdcardfs): checks the build is present and the kernel can
# serve it, sets permissions, and prints the one line described at the bottom
# (the storage path — not readable anywhere before the first boot).
#
# Общий шаг установщика (права на дерево, уборка мёртвого конфига) и проверка
# zygisk-плеча лежат в module/common/lib.sh — том же файле, что уходит в архив и
# подключается обеими сборками.
#

SKIPUNZIP=0

. "$MODPATH/lib.sh"

case "$ARCH" in
    arm64)   ABI=arm64-v8a   ;;
    arm)     ABI=armeabi-v7a ;;
    *)       abort "! Unsupported architecture: $ARCH (arm64-v8a or armeabi-v7a required)" ;;
esac

unfuse_require_zygisk_so "$ABI"

# The description line in module.prop gets a status prefix written into it at
# boot (status.sh). The shipped module.prop already carries the plain text, so
# a fresh install starts with a description that is simply missing its status
# rather than one left over from the build machine.
unfuse_install_common "$ABI"
set_perm "$MODPATH/status.sh" 0 0 0755 2>/dev/null

# --- kernel prerequisite ------------------------------------------------------
#
# The module's only path is sdcardfs: it mounts /mnt/runtime/*/emulated itself
# and hands those points to Zygote. Without the driver in the kernel there is
# nothing to mount and nothing to substitute, so the install is refused here
# rather than leaving a module that boots and leaves apps without storage.
#
if ! grep -qw sdcardfs /proc/filesystems 2>/dev/null; then
    ui_print "- sdcardfs НЕ НАЙДЕН в /proc/filesystems"
    abort "! Это ядро не умеет sdcardfs — модуль на нём работать не будет"
fi

# --- one line of output ------------------------------------------------------
#
# The installer says one thing about the storage path: that the kernel has
# sdcardfs, and the module will mount it on /mnt/runtime/*/emulated. What the
# module does and how it is doing is in the description of module.prop, which
# service.sh rewrites with a status prefix on every boot; printing it here as
# well would only bury the one fact that cannot be read there before the first
# boot.
#
ui_print "- Storage path: sdcardfs (kernel has sdcardfs)"

# --- and nothing about the storage form ---------------------------------------
#
# On 11 the form is a boot property (persist.sys.fuse), and the module used to
# print what it would do about it. The property is not decided by the module —
# init.rc:792 sets it, load_persist_props restores it from the store after every
# module script has run, and the Zygisk arm serves both forms either way (see
# post-fs-data.sh) — so a line here would describe a decision the module does not
# make.
#
# The pair of properties is still shipped, in system.prop: the loader applies it
# after the module's post-fs-data.sh and, being a direct write (resetprop -n in
# both Magisk and KernelSU), it does not reach the store. What that can and cannot
# achieve is in docs/android-11-persist-fuse-prop.md, §5.2. Nothing about it is
# printed here either: it is not a promise, it is an attempt, and the module list
# shows what actually happened only as far as status.sh can see it (the kernel).
