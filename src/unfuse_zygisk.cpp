/*
 * Unfuse Zygisk — a Zygisk module restoring Android 10 internal-storage
 * behaviour: direct access to all storage for ALL apps, no FUSE, no
 * MediaProvider in the data path. Design: docs/fuse-root-patch-design.md.
 *
 * The module mounts nothing: tools/vold-fusefs redirects vold's mount(), so
 * MountUserFuse() binds /data/media on the shared /mnt/user/<user>/emulated,
 * reaching every app mount namespace by propagation. Zygote then binds
 * /mnt/user/<user> onto /storage; left over is not covering Android/{data,obb}
 * with a per-package tmpfs. Which processes that reaches is decided by
 * clears_storage_dirs(): every mode the framework isolates, not just the two
 * the module used to know about.
 *
 * The module says nothing: no logcat line, no journal. What is left to observe
 * it by is the boot marker below — a file, not a message — plus the patch state,
 * which tools/status.sh and the device harnesses read.
 *
 * Kill switch: /data/adb/unfuse_zygisk.state/no_hooks, read in preAppSpecialize
 * while still root, since /data/adb is invisible to apps. Fails open.
 *
 * State files need a directory of their own: over /data/adb the zygote domain
 * has traversal and nothing else (allow zygote adb_data_file dir search), so
 * open(O_CREAT) there fails on add_name and dontaudit hides it; STATE_DIR
 * carries magisk_file — the one type policy grants every domain (Zygisk Next).
 */

#include <fcntl.h>
#include <jni.h>
#include <unistd.h>

#include "hook_libc.h"
#include "zygisk.hpp"

namespace {

// Created and labelled by post-fs-data.sh before Zygote starts — the module
// cannot create it (see the header). A macro, so both paths are compile-time
// joins.
#define STATE_DIR "/data/adb/unfuse_zygisk.state"

constexpr const char *kNoHooksFlag = STATE_DIR "/no_hooks";

// The module's only trace: claimed by the first app with storage, removed by
// post-fs-data.sh at boot. Its presence says the module reached
// postAppSpecialize in this boot. Nothing is written into it and nothing inside
// the module reads it back — it is evidence, not state.
constexpr const char *kOnceFlag = STATE_DIR "/once";

// Creates path if absent. Several apps specialise at once, so the creation must
// be atomic, hence O_EXCL; EEXIST is the ordinary outcome for every app but the
// first. A state directory that is missing or unreachable leaves the marker
// absent — indistinguishable from "the module never ran", which is why the
// marker is only ever read together with the patch state.
void claim_once(const char *path) {
    const int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd >= 0) close(fd);
}

// android.os.storage.StorageManager.MOUNT_MODE_EXTERNAL_*, which is IVold's
// REMOUNT_MODE_* and Zygote's own MountExternalKind copy of it. The numbering is
// not stable across releases (IVold.aidl, 11 through 17):
//
//   11  : NONE 0, DEFAULT 1, READ 2, WRITE 3, LEGACY 4, INSTALLER 5, FULL 6,
//         PASS_THROUGH 7, ANDROID_WRITABLE 8
//   12+ : NONE 0, DEFAULT 1, INSTALLER 2, PASS_THROUGH 3, ANDROID_WRITABLE 4
//
// Accepted are the modes whose mount_storage_dirs the framework may have set —
// all of them except the three it excludes itself in ProcessList.
// needsStorageDataIsolation() on every release: NONE, INSTALLER, PASS_THROUGH.
//
//   DEFAULT         1 on both. The ordinary app.
//   READ / WRITE    2 and 3, 11 only. Legacy storage: an app granted
//                   OP_LEGACY_STORAGE (StorageManagerService.getMountModeInternal
//                   — hasLegacy && hasWrite -> WRITE, && hasRead -> READ). These
//                   are the apps that want the raw tree most, and until now the
//                   module declined them: on a ROM with app data isolation on
//                   they kept the per-package tmpfs over Android/{data,obb}.
//   FULL            6, 11 only. WRITE_MEDIA_STORAGE, i.e. privileged apps.
//   ANDROID_WRITABLE 8 on 11, 4 from 12 — the SAF provider behind "Files".
//
// LEGACY 4 is never handed out (getMountModeInternal has no LEGACY return), so
// accepting 4 costs nothing on 11 and is what 12+ numbers ANDROID_WRITABLE.
// On 12+ the numbers 2 and 3 are INSTALLER and PASS_THROUGH, whose
// mount_storage_dirs the framework already leaves false — the write is a no-op —
// and 6 is past COUNT 5, so it never arrives.
constexpr int kMountModeExternalDefault = 1;
constexpr int kMountModeExternalRead = 2;              // 11 only
constexpr int kMountModeExternalWrite = 3;             // 11 only
constexpr int kMountModeExternalAndroidWritable = 4;   // 12 and up
constexpr int kMountModeExternalFull = 6;              // 11 only
constexpr int kMountModeExternalAndroidWritableR = 8;  // 11 only

bool is_android_writable(int mode) {
    return mode == kMountModeExternalAndroidWritable ||
           mode == kMountModeExternalAndroidWritableR;
}

// Every mode whose mount_storage_dirs the framework may have set. A mode left
// out of this is one the framework does not isolate, so there is nothing to
// clear for it — which is also why no mode can be taken by mistake here: the
// module's action does not depend on which mode it is, it only ever clears one
// boolean.
bool clears_storage_dirs(int mode) {
    return mode == kMountModeExternalDefault ||
           mode == kMountModeExternalRead ||
           mode == kMountModeExternalWrite ||
           mode == kMountModeExternalFull ||
           is_android_writable(mode);
}

class UnfuseZygisk : public zygisk::ModuleBase {
public:
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        const int mode = args->mount_external;
        // Only the modes the framework isolates are touched — NONE, INSTALLER
        // and PASS_THROUGH have nothing to clear. No app list: every
        // Zygote-started process is covered.
        if (!clears_storage_dirs(mode)) return;

        // Zygote must not cover Android/{data,obb} with a per-package tmpfs:
        // the raw tree arrives as vold's bind. The module's own bind and its
        // MS_REC|MS_PRIVATE are gone: privatisation severed the slave link
        // vold's bind reaches app namespaces through. Nothing covers
        // /mnt/runtime/<view>/emulated on the pre-FUSE shape (persist.sys.fuse
        // false, 11/12/12L/13) — MountUserFuse() mounts only under
        // /mnt/user/<user> (Utils.cpp:1600-1602), /mnt/runtime/<view> is
        // sdcardfs's (EmulatedVolume.cpp:360-363).
        if (args->mount_storage_dirs != nullptr) {
            *args->mount_storage_dirs = JNI_FALSE;
        }

        // Absence means "hooks on": a state directory that cannot be read is not
        // evidence that the user wanted the hooks off, and switching them off on
        // that basis would turn a broken install into a quiet no-op.
        hooks_allowed_ = (access(kNoHooksFlag, F_OK) != 0);

        claim_once(kOnceFlag);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!hooks_allowed_) return;

        // hook_libc.cpp's patch runs here: after specialisation it cannot
        // leak into zygote as it would from preAppSpecialize, and it runs on
        // the raw source. The tally is left to hookselftest.cpp, which is where
        // it can be read at all.
        hooks_install(nullptr);
    }

private:
    // State survives specialisation: both callbacks run on the same instance.
    bool hooks_allowed_ = true;
};

}  // namespace

REGISTER_ZYGISK_MODULE(UnfuseZygisk)
