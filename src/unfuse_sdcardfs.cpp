/*
 * Unfuse Zygisk — a Zygisk module restoring Android 10 internal-storage
 * behaviour: direct access to all storage for ALL apps, no FUSE, no
 * MediaProvider in the data path.
 *
 * (1) Places the storage source under the point Zygote is about to bind onto
 * /storage, in the process's private mount namespace, so /storage/emulated is
 * substituted too. Which point that is depends on the release: 14 and up always
 * bind /mnt/user/<user>, while 11, 12, 12L and 13 have a second arm that binds
 * /mnt/runtime/<view> whenever persist.sys.fuse is not true. Both are covered —
 * see zygote_uses_runtime_view(). (2) Clears *args->mount_storage_dirs so
 * Zygote does not cover Android/{data,obb} with a per-package tmpfs, giving full
 * Android/data and Android/obb as before scoped storage.
 *
 * The source is /mnt/runtime/full/emulated — sdcardfs, mask 0007, gid 9997 ->
 * 0770/0660, raised by storage.sh since this ROM has
 * external_storage.sdcardfs.enabled=0 and vold does not mount it. There is no
 * second source: the installer refuses the module on a kernel without sdcardfs,
 * so the bind is checked by fs type and a mismatch is reported rather than
 * worked around.
 *
 * DEFAULT and ANDROID_WRITABLE (SAF provider com.android.externalstorage behind
 * the system "Files" app) are handled; NONE, INSTALLER and PASS_THROUGH are
 * skipped. No app list: every Zygote-started process is covered. ANDROID_WRITABLE
 * is the one mode number that is not the same on every release — 8 on 11, 4 from
 * 12 on — so both are accepted; see is_android_writable().
 *
 * The module reports nothing and writes nothing to disk: a bind that does not
 * take is silent, and Zygote is simply left to do what it would have done
 * without the module.
 */

#include <jni.h>
#include <sched.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/statfs.h>
#include <sys/system_properties.h>

#include <string>

#include "zygisk.hpp"

namespace {

// The source: sdcardfs with full access (mask=0007, gid=9997).
constexpr const char *kSourceSdcardfs = "/mnt/runtime/full/emulated";

// android.os.storage.StorageManager.MOUNT_MODE_EXTERNAL_*, which is
// IVold.REMOUNT_MODE_* and Zygote's own MountExternalKind copy of it.
//
// DEFAULT is 1 on every release. ANDROID_WRITABLE is NOT: 11 still carries the
// long enum (READ, WRITE, LEGACY, FULL) and numbers it 8, while 12 dropped
// those and renumbered it to 4. Both are accepted here instead of branching on
// the release, because the two values cannot collide. 4 is LEGACY on 11, and
// 11's StorageManagerService never hands LEGACY out — only NONE, DEFAULT, READ,
// WRITE, INSTALLER, FULL and ANDROID_WRITABLE leave getMountMode(), and the two
// ExternalStorageMountPolicy implementations return nothing outside NONE,
// DEFAULT, READ and WRITE. 8 is past the end of the 12+ enum, so it cannot
// occur there. A release that renumbers again shows up as an android_writable
// process the module declines.
constexpr int kMountModeExternalDefault = 1;
constexpr int kMountModeExternalAndroidWritable = 4;      // 12 and up
constexpr int kMountModeExternalAndroidWritableR = 8;     // 11 only

bool is_android_writable(int mode) {
    return mode == kMountModeExternalAndroidWritable ||
           mode == kMountModeExternalAndroidWritableR;
}

// AID_USER_OFFSET from android_filesystem_config.h
constexpr unsigned kAidUserOffset = 100000;

// SDCARDFS_SUPER_MAGIC (include/uapi/linux/magic.h)
constexpr unsigned long kSdcardFsMagic = 0x5dca2df5UL;

// android::base::GetBoolProperty: the same set of spellings it accepts, and the
// caller's fallback when the property is unset or unreadable.
bool prop_bool(const char *name, bool fallback) {
    char v[PROP_VALUE_MAX];
    if (__system_property_get(name, v) <= 0) return fallback;
    static const char *const kTrue[] = {"1", "y", "yes", "on", "true", "t"};
    static const char *const kFalse[] = {"0", "n", "no", "off", "false", "f"};
    for (const char *s : kTrue) {
        if (strcasecmp(v, s) == 0) return true;
    }
    for (const char *s : kFalse) {
        if (strcasecmp(v, s) == 0) return false;
    }
    return fallback;
}

// Zygote's MountEmulatedStorage() has two arms, and the module has to place the
// source where the arm that will actually run is going to look.
//
// 11 (and 12/12L/13) keeps the pre-FUSE shape: when persist.sys.fuse is not
// true, Zygote binds ExternalStorageViews[mount_mode] — /mnt/runtime/<view> —
// onto /storage, and /mnt/user/<user> only onto /storage/self. The
// substitution at /mnt/user/<user>/emulated is then off the path entirely.
// 14 dropped that arm: /mnt/user/<user> is bound onto /storage unconditionally,
// so the runtime view never matters there.
//
// Rather than guess the ROM, mirror Zygote's own test — the same property, the
// same default it reads (false; vold reads the same name with a default of
// true, which is why the two can disagree on a device that sets neither).
bool zygote_uses_runtime_view() { return !prop_bool("persist.sys.fuse", false); }

// ExternalStorageViews[] in that arm, restricted to the two modes the module
// handles. ANDROID_WRITABLE maps to /mnt/runtime/full, which is the sdcardfs
// source itself, so the caller skips it when the destination already is that
// source.
const char *runtime_view_for_mode(int mode) {
    if (mode == kMountModeExternalDefault) return "default";
    if (is_android_writable(mode)) return "full";
    return nullptr;
}

void rollback(const std::string &dst) { umount2(dst.c_str(), MNT_DETACH); }

// Binds the sdcardfs source onto dst; true only if it is really sdcardfs under
// the point afterwards. What matters is the fs type, not mount(2) success:
// storage.sh failing to raise its mounts would leave an empty directory under
// the point, and Zygote would bind that onto /storage. statfs() does not touch
// the inode, so unlike stat() it cannot be denied by directory permissions.
bool attach_sdcardfs(const std::string &dst) {
    if (mount(kSourceSdcardfs, dst.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        return false;
    }

    struct statfs st {};
    if (statfs(dst.c_str(), &st) != 0) {
        rollback(dst);
        return false;
    }

    if (static_cast<unsigned long>(st.f_type) != kSdcardFsMagic) {
        rollback(dst);
        return false;
    }
    return true;
}

class UnfuseZygisk : public zygisk::ModuleBase {
public:
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        const int mode = args->mount_external;
        const bool android_writable = is_android_writable(mode);
        if (mode != kMountModeExternalDefault && !android_writable) return;

        // Private namespace copy: otherwise the mount leaks into zygote and all
        // its children. unshare() also copies propagation settings, so make the
        // whole tree private right away.
        if (unshare(CLONE_NEWNS) != 0) return;
        if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) return;

        const std::string user =
            std::to_string(static_cast<unsigned>(args->uid) / kAidUserOffset);

        // Zygote takes whatever point it finds under /storage: usually
        // /mnt/user/<user>/emulated, or /mnt/androidwritable/<user>/emulated for
        // ANDROID_WRITABLE with persist.sys.vold_app_data_isolation_enabled.
        bool ok = attach_sdcardfs("/mnt/user/" + user + "/emulated");

        if (android_writable && attach_sdcardfs("/mnt/androidwritable/" + user + "/emulated")) {
            ok = true;
        }

        // The other arm of Zygote's MountEmulatedStorage(): when it is going to
        // bind /mnt/runtime/<view> onto /storage instead of /mnt/user/<user>,
        // the substitution above is off the path. Put the source there as well.
        // Both binds are kept: /storage/self comes from /mnt/user/<user> even on
        // that arm, so dropping the first would lose it.
        if (zygote_uses_runtime_view()) {
            const char *view = runtime_view_for_mode(mode);
            if (view != nullptr) {
                const std::string dst =
                    std::string("/mnt/runtime/") + view + "/emulated";
                // ANDROID_WRITABLE's view is /mnt/runtime/full, which IS the
                // sdcardfs source: binding it onto itself adds nothing.
                if (dst != kSourceSdcardfs && attach_sdcardfs(dst)) {
                    ok = true;
                }
            }
        }
        if (!ok) return;

        if (args->mount_storage_dirs != nullptr) {
            *args->mount_storage_dirs = JNI_FALSE;
        }
    }
};

}  // namespace

REGISTER_ZYGISK_MODULE(UnfuseZygisk)
