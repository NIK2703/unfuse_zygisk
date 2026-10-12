/*
 * Unfuse Zygisk — a Zygisk module restoring Android 10 internal-storage
 * behaviour: direct access to all storage for ALL apps, no FUSE, no
 * MediaProvider in the data path.
 *
 * (1) Places the storage source under the point Zygote is about to bind onto
 * /storage, in the process's private mount namespace, so /storage/emulated is
 * substituted too. Which point that is depends on the release AND on the
 * storage form — Zygote's MountEmulatedStorage() has two arms and the ROM picks
 * one by property. Both arms are served, at once, from the table below: see
 * kPoints and the header of preAppSpecialize(). The SD card is served the same
 * way, one component deeper: see attach_card_points(). (2) Clears
 * *args->mount_storage_dirs so Zygote does not cover Android/{data,obb} with a
 * per-package tmpfs, giving full Android/data and Android/obb as before scoped
 * storage.
 *
 * The source is /mnt/runtime/full/emulated — sdcardfs, mask 0007, gid 9997 ->
 * 0770/0660. storage.sh raises it at post-fs-data, and vold raises its own on
 * the same points whenever external_storage.sdcardfs.enabled (Utils.cpp:1006)
 * is not 0 — the two carry the same masks (sdcard.cpp), and vold's lands on
 * top, so the source survives either way. There is no second source: the
 * installer refuses the module on a kernel without sdcardfs, so the bind is
 * checked by fs type and a mismatch is reported rather than worked around.
 *
 * persist.sys.fuse is NOT read here, and that is the point. The property
 * decides which arm Zygote takes, and the module used to switch on it — but the
 * value belongs to the ROM: init.rc:792 sets it true in on post-fs-data, and
 * load_persist_props (init.rc:437) restores whatever the store holds after
 * every module script has run, so post-fs-data.sh could never make it stick.
 * Depending on a value the module cannot set was the bug; serving both arms
 * removes the dependence instead of fighting for the value.
 *
 * No app list: every Zygote-started process is covered.
 *
 * The module reports nothing and writes nothing to disk: a bind that does not
 * take is silent, and Zygote is simply left to do what it would have done
 * without the module.
 */

#include <dirent.h>
#include <jni.h>
#include <sched.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/statfs.h>

#include <cstring>
#include <string>

#include "zygisk.hpp"

namespace {

// The source: sdcardfs with full access (mask=0007, gid=9997).
constexpr const char *kSourceSdcardfs = "/mnt/runtime/full/emulated";

// android.os.storage.StorageManager.MOUNT_MODE_EXTERNAL_*, which is IVold.
// REMOUNT_MODE_* and Zygote's own MountExternalKind copy of it.
constexpr int kMountModeExternalNone = 0;
constexpr int kMountModeExternalDefault = 1;
constexpr int kMountModeExternalRead = 2;
constexpr int kMountModeExternalWrite = 3;
constexpr int kMountModeExternalLegacy = 4;            // LEGACY on 11, ANDROID_WRITABLE from 12
constexpr int kMountModeExternalInstaller = 5;
constexpr int kMountModeExternalFull = 6;
constexpr int kMountModeExternalPassThrough = 7;
constexpr int kMountModeExternalAndroidWritable = 8;   // 11 only

// AID_USER_OFFSET from android_filesystem_config.h
constexpr unsigned kAidUserOffset = 100000;

// SDCARDFS_SUPER_MAGIC (include/uapi/linux/magic.h)
constexpr unsigned long kSdcardFsMagic = 0x5dca2df5UL;

// Zygote's MountEmulatedStorage() (Zygote.cpp:801-858) binds exactly one point
// onto /storage, and which one depends on the storage form:
//
//   FUSE arm — persist.sys.fuse true, Zygote.cpp:837-850. Everything except
//   PASS_THROUGH and INSTALLER gets /mnt/user/<user>; INSTALLER gets
//   /mnt/installer/<user>; ANDROID_WRITABLE gets /mnt/androidwritable/<user>
//   when app-data isolation is on.
//
//   sdcardfs arm — Zygote.cpp:851-857, ExternalStorageViews[mount_mode]
//   (Zygote.cpp:329-338).
//
// The module places the source at every point its mode can land on. The arm
// that runs finds it; the other arm's points are left unused, which costs one
// VFS mount and nothing else. That is the whole reason the module no longer
// needs to know the property's value.
//
// Every mode gets the same source, so a READ-mode process sees the writable
// view where the ROM would have given it a read-only one (the stock masks are
// 6/23/7/7 for default/read/write/full, the module's is 7 for all four). That is
// the module's promise — direct access to all storage for all apps — and not a
// side effect of the table: the source is one, and there is deliberately no
// second one (see the file header).
//
// PASS_THROUGH is the one mode with no row: on the FUSE arm vold fills
// /mnt/pass_through/<user> from /mnt/runtime/full (Utils.cpp:1568-1573), and on
// the other arm its view IS /mnt/runtime/full. Both are sdcardfs already.
//
// %u is the user id. /mnt/runtime/full/emulated is the source itself and is
// therefore never a destination — binding it onto itself would add nothing.
struct Point {
    int mode;
    const char *path;
};

constexpr Point kPoints[] = {
    // DEFAULT, READ, WRITE — the ordinary app modes. FUSE arm: /mnt/user.
    // sdcardfs arm: ExternalStorageViews[1..3].
    {kMountModeExternalDefault, "/mnt/user/%u/emulated"},
    {kMountModeExternalDefault, "/mnt/runtime/default/emulated"},
    {kMountModeExternalRead, "/mnt/user/%u/emulated"},
    {kMountModeExternalRead, "/mnt/runtime/read/emulated"},
    {kMountModeExternalWrite, "/mnt/user/%u/emulated"},
    {kMountModeExternalWrite, "/mnt/runtime/write/emulated"},

    // Mode 4 is two different things. On 11 it is LEGACY, which no
    // ExternalStorageMountPolicy returns and StorageManagerService therefore
    // never hands out; from 12 on it is ANDROID_WRITABLE, which 11 numbers 8.
    // The two meanings cannot collide, so one row carries both and is right on
    // either release. A release that renumbers again shows up as a process
    // whose mode is in no row, and the module declines it.
    {kMountModeExternalLegacy, "/mnt/user/%u/emulated"},             // LEGACY / ANDROID_WRITABLE
    {kMountModeExternalLegacy, "/mnt/runtime/write/emulated"},       // LEGACY
    {kMountModeExternalLegacy, "/mnt/androidwritable/%u/emulated"},  // ANDROID_WRITABLE

    // INSTALLER: the FUSE arm binds /mnt/installer, the other arm
    // ExternalStorageViews[5] = /mnt/runtime/write. Installer sessions are apps
    // like any other, so they get the same tree.
    {kMountModeExternalInstaller, "/mnt/installer/%u/emulated"},
    {kMountModeExternalInstaller, "/mnt/runtime/write/emulated"},

    // FULL: only /mnt/user is left, since the sdcardfs arm's view for it is the
    // source itself.
    {kMountModeExternalFull, "/mnt/user/%u/emulated"},

    // ANDROID_WRITABLE, 11's numbering: same as the mode-4 row plus 11's own
    // value.
    {kMountModeExternalAndroidWritable, "/mnt/user/%u/emulated"},
    {kMountModeExternalAndroidWritable, "/mnt/androidwritable/%u/emulated"},
};

void rollback(const std::string &dst) { umount2(dst.c_str(), MNT_DETACH); }

// Binds src onto dst; true only if it is really sdcardfs under the point
// afterwards. What matters is the fs type, not mount(2) success: storage.sh
// failing to raise its mounts, or a point that does not exist at all
// (ENOENT — the ordinary case for the arm that is not in use), would otherwise
// pass for a substitution. statfs() does not touch the inode, so unlike stat()
// it cannot be denied by directory permissions.
//
// The source is an argument because the card is a second one — see
// attach_card_points below.
bool attach_source(const char *src, const char *dst) {
    if (mount(src, dst, nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        return false;
    }

    struct statfs st {};
    if (statfs(dst, &st) != 0) {
        rollback(dst);
        return false;
    }

    if (static_cast<unsigned long>(st.f_type) != kSdcardFsMagic) {
        rollback(dst);
        return false;
    }
    return true;
}

bool attach_sdcardfs(const char *dst) {
    return attach_source(kSourceSdcardfs, dst);
}

// ---- the SD card -----------------------------------------------------------
//
// The public volume is served exactly like the emulated point, one component
// deeper: vold mounts sdcardfs for it at /mnt/runtime/{default,read,write,full}/
// <stableName> — the same four views with the same masks (PublicVolume.cpp:126-129
// names them, the sdcardfs fork at :166-230 runs /system/bin/sdcard with -u/-g
// 1023) — and on the FUSE arm the app's path to the card is
// /mnt/user/<user>/<stableName>, because Zygote binds /mnt/user/<user> onto
// /storage (Zygote.cpp:849) and the card sits under it by its stableName
// (PublicVolume.cpp:135-139 setPath, MountUserFuse writes
// /mnt/user/<user>/<stableName>).
//
// So every kPoints row this mode can land on has a card row under it: the
// row's path with its trailing "/emulated" replaced by the card's stableName.
// The name is the volume's fsUuid (PublicVolume.cpp:118-122) — different per
// card and unknowable ahead of time — so there is no table: the names are read
// from the mount points at each app start, which is exactly when they are
// needed and the only moment there is to read them. A card that appears in one
// name dir appears in all of them (it is one volume), so which dirs are read
// is only about coverage, and several are read on purpose: see kCardNameDirs.
// No card mounted and every dir holds only its fixed entries, so the loop does
// nothing; a card inserted while apps run is picked up by every app that
// starts afterwards, which is the same freshness model the emulated arm has
// (it is read from the mount table at the same moment).
//
// What this fixes is the mode mask: an app in DEFAULT/READ/WRITE would
// otherwise reach the card through its view's mask (the stock 6/23/7 of the
// four views, against the module's 7 for all four) and fail to write where the
// module's promise says it must not. FUSE is not in this arm's way to fix:
// with persist.sys.fuse false (the module's system.prop) vold never mounts FUSE
// for the card at all — PublicVolume.cpp:230-236 leaves the sdcardfs views as
// the only ones — and where FUSE is mounted (12+, or an 11 whose store holds
// the pair true) the bind goes over the FUSE mount in the app's own namespace,
// which is the same thing the emulated rows do for /mnt/user/<user>/emulated,
// and the fs-type check rejects it if it does not take.
//
// Deliberately independent of the emulated result: the card is a separate
// volume. A card that took is not rolled back because the emulated rows found
// nothing, and it does not become a reason to clear mount_storage_dirs — that
// flag belongs to the emulated model, and clearing it while the emulated bind
// did not take is the half-applied state the file header warns about.
//
// Where the card is read from is not one dir but several, and why matters.
// First the names: the fsUuid appears in every view dir and in the raw dir
// alike (PublicVolume.cpp:124-129 builds all five paths from one string), so a
// card mounted normally leaves its name everywhere; the ROM may nonetheless
// skip a dir — with external_storage.sdcardfs.enabled=0 PublicVolume:161-164
// returns before the views exist and only the raw is mounted — so the name
// search walks every dir and each name is used once. "self" is skipped: init.rc
// creates it as a plain 0755 directory under every view (init.rc:215-221), it
// is not a volume, and binding it would only fail the fs-type check.
// Then the source: the full view is what the module promises, but it is the one
// dir the sdcardfs path may not have, while the raw — /mnt/media_rw/<name>,
// mounted by PublicVolume.cpp:143-155 with gid AID_MEDIA_RW and mask 0007 —
// exists for every visible volume regardless. The source candidates are tried
// in order and the first real filesystem wins. Because a card may be ntfs or
// exfat, the check accepts the card filesystems, not just sdcardfs: the point
// is that a real filesystem is under dst (a missed bind rolls back and leaves
// Zygote's own view alone), and the emulated arm — the one the installer
// refuses to run without sdcardfs — keeps its own strict check untouched.
constexpr const char *kCardNameDirs[] = {
    "/mnt/runtime/full",
    "/mnt/runtime/write",
    "/mnt/media_rw",
    "/mnt/runtime/default",
    "/mnt/runtime/read",
};
constexpr const char *kCardSrcDirs[] = {
    "/mnt/runtime/full",
    "/mnt/media_rw",
};
constexpr int kCardNamesMax = 16;   // cards a device can carry at once
constexpr int kCardNameLen = 48;    // enough for any fsUuid (max 36 + slack)

// Filesystems a card source may be: the sdcardfs view the ROM mounts, or the
// raw filesystem of the card itself (PublicVolume.cpp:143-155 mounts vfat/exfat
// at /mnt/media_rw; the others a device may carry). SDCARDFS_SUPER_MAGIC,
// MSDOS_SUPER_MAGIC (vfat), EXFAT_SUPER_MAGIC, F2FS_SUPER_MAGIC, NTFS magic.
constexpr unsigned long kCardFsMagics[] = {
    0x5dca2df5UL,  // sdcardfs
    0x00004d44UL,  // vfat
    0x00005df5UL,  // exfat
    0xf2f52010UL,  // f2fs
    0x5346544eUL,  // ntfs
};

bool is_card_fs(unsigned long type) {
    for (unsigned long magic : kCardFsMagics) {
        if (type == magic) return true;
    }
    return false;
}

// Like attach_source, but for the card: any real card filesystem counts, not
// only sdcardfs, because the card's full view may be missing while its raw
// exists. Same verdict discipline — true only if a filesystem is under dst.
bool attach_card_source(const char *src, const char *dst) {
    if (mount(src, dst, nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        return false;
    }
    struct statfs st {};
    if (statfs(dst, &st) != 0 ||
        !is_card_fs(static_cast<unsigned long>(st.f_type))) {
        rollback(dst);
        return false;
    }
    return true;
}

bool attach_card_points(int mode, unsigned user) {
    // Collect the volume names first. One card shows up in every name dir, so
    // the set keeps each name at one entry and the mount work below once per
    // card, whichever dir it was found in.
    char names[kCardNamesMax][kCardNameLen];
    int count = 0;

    for (const char *dir : kCardNameDirs) {
        DIR *d = opendir(dir);
        if (d == nullptr) continue;
        struct dirent *de;
        while (count < kCardNamesMax && (de = readdir(d)) != nullptr) {
            const char *name = de->d_name;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
                strcmp(name, "emulated") == 0 || strcmp(name, "self") == 0) {
                continue;
            }
            bool known = false;
            for (int i = 0; i < count; ++i) {
                if (strcmp(names[i], name) == 0) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                snprintf(names[count], kCardNameLen, "%s", name);
                ++count;
            }
        }
        closedir(d);
    }

    bool any = false;
    for (int i = 0; i < count; ++i) {
        const char *name = names[i];

        for (const Point &p : kPoints) {
            if (p.mode != mode) continue;
            // The row's path ends in "/emulated"; the card's stableName takes
            // exactly its place, so the base is everything before that slash.
            const char *tail = strstr(p.path, "/emulated");
            if (tail == nullptr) continue;
            char base[64];
            const size_t blen = static_cast<size_t>(tail - p.path);
            if (blen == 0 || blen >= sizeof(base)) continue;
            memcpy(base, p.path, blen);
            base[blen] = '\0';

            char dst[128];
            char formatted[64];
            const int fn = snprintf(formatted, sizeof(formatted), base, user);
            if (fn <= 0 || static_cast<unsigned>(fn) >= sizeof(formatted)) continue;
            const int dn = snprintf(dst, sizeof(dst), "%s/%s", formatted, name);
            if (dn <= 0 || static_cast<unsigned>(dn) >= sizeof(dst)) continue;

            // Sources in order, first real filesystem wins. A source equal to
            // dst would bind it onto itself; no kPoints row is, but the guard
            // costs one strcmp.
            for (const char *srcdir : kCardSrcDirs) {
                char src[128];
                const int sn = snprintf(src, sizeof(src), "%s/%s", srcdir, name);
                if (sn <= 0 || static_cast<unsigned>(sn) >= sizeof(src)) continue;
                if (strcmp(src, dst) == 0) continue;
                if (attach_card_source(src, dst)) {
                    any = true;
                    break;
                }
            }
        }
    }
    return any;
}

class UnfuseZygisk : public zygisk::ModuleBase {
public:
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        const int mode = args->mount_external;

        // NONE: Zygote returns before binding anything onto /storage
        // (Zygote.cpp:811-814), so there is no point to substitute — and a
        // process that asked for no storage must not be given any.
        // PASS_THROUGH: sdcardfs on both arms already, see kPoints.
        if (mode == kMountModeExternalNone || mode == kMountModeExternalPassThrough) return;

        // Private namespace copy: otherwise the mount leaks into zygote and all
        // its children. unshare() also copies propagation settings, so make the
        // whole tree private right away.
        if (unshare(CLONE_NEWNS) != 0) return;
        if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) return;

        const unsigned user = static_cast<unsigned>(args->uid) / kAidUserOffset;

        // Every point this mode can land on, on either arm. One of them is where
        // Zygote is about to look; a point that is not there fails with ENOENT
        // and a point that is already something else fails the fs-type check, so
        // "any of them took" is exactly "the path is covered".
        bool ok = false;
        for (const Point &p : kPoints) {
            if (p.mode != mode) continue;
            char dst[64];
            const int n = snprintf(dst, sizeof(dst), p.path, user);
            if (n <= 0 || static_cast<unsigned>(n) >= sizeof(dst)) continue;
            if (attach_sdcardfs(dst)) ok = true;
        }

        // The SD card, on its own verdict: it does not vote for ok and does not
        // get rolled back by it — the emulated row set and the card rows are
        // separate volumes (see the comment above attach_card_points).
        attach_card_points(mode, user);

        if (!ok) return;

        if (args->mount_storage_dirs != nullptr) {
            *args->mount_storage_dirs = JNI_FALSE;
        }
    }
};

}  // namespace

REGISTER_ZYGISK_MODULE(UnfuseZygisk)
