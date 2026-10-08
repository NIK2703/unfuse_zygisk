/*
 * Unfuse Zygisk — a Zygisk module restoring Android 10 internal-storage
 * behaviour: direct access to all storage for ALL apps, no FUSE, no
 * MediaProvider in the data path. Design: docs/fuse-root-patch-design.md.
 *
 * The module mounts nothing: tools/vold-fusefs redirects vold's mount(), so
 * MountUserFuse() binds /data/media on the shared /mnt/user/<user>/emulated,
 * reaching every app mount namespace by propagation. Zygote then binds
 * /mnt/user/<user> onto /storage; left over is not covering Android/{data,obb}
 * with a per-package tmpfs. ANDROID_WRITABLE is 8 on 11 and 4 from 12, both
 * accepted: see is_android_writable().
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

// android.os.storage.StorageManager.MOUNT_MODE_EXTERNAL_*, which is IVold.
// REMOUNT_MODE_* and Zygote's own MountExternalKind copy of it. DEFAULT is 1
// on every release; ANDROID_WRITABLE — the SAF provider behind the system
// "Files" app — is not: 11 numbers it 8 (long enum, READ/WRITE/LEGACY/FULL),
// 12 dropped those and renumbered it to 4. Both are accepted because the two
// values cannot collide: 4 is LEGACY on 11, which no ExternalStorageMountPolicy
// returns, and 8 is past the end of the 12+ enum. A renumbering release shows up
// as an android_writable process the module declines — and then as storage still
// isolated in that process, which is the only place it can show up at all.
constexpr int kMountModeExternalDefault = 1;
constexpr int kMountModeExternalAndroidWritable = 4;      // 12 and up
constexpr int kMountModeExternalAndroidWritableR = 8;     // 11 only

bool is_android_writable(int mode) {
    return mode == kMountModeExternalAndroidWritable ||
           mode == kMountModeExternalAndroidWritableR;
}

class UnfuseZygisk : public zygisk::ModuleBase {
public:
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        const int mode = args->mount_external;
        // NONE, INSTALLER and PASS_THROUGH are skipped as raw /data/media
        // consumers; no app list, every Zygote-started process is covered.
        if (mode != kMountModeExternalDefault && !is_android_writable(mode)) return;

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
