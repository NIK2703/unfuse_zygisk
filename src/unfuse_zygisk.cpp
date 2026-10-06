/*
 * Unfuse Zygisk — a Zygisk module restoring Android 10 internal-storage
 * behaviour: direct access to all storage for ALL apps, no FUSE, no
 * MediaProvider in the data path.
 *
 * (1) Places a storage source under the point Zygote is about to bind onto
 * /storage, in the process's private mount namespace, so /storage/emulated is
 * substituted too. Which point that is depends on the release: 14 and up always
 * bind /mnt/user/<user>, while 11, 12, 12L and 13 have a second arm that binds
 * /mnt/runtime/<view> whenever persist.sys.fuse is not true. Both are covered —
 * see zygote_uses_runtime_view(). (2) Clears *args->mount_storage_dirs so
 * Zygote does not cover Android/{data,obb} with a per-package tmpfs, giving full
 * Android/data and Android/obb as before scoped storage.
 *
 * Source chosen at runtime: primary /mnt/runtime/full/emulated — sdcardfs, mask
 * 0007, gid 9997 -> 0770/0660, raised by storage.sh since this ROM has
 * external_storage.sdcardfs.enabled=0 and vold does not mount it; fallback raw
 * /data/media when the kernel lacks sdcardfs (the path AOSP exposes at
 * /mnt/pass_through; storage.sh adds a named 9997 ACL entry, else it is
 * 1023:1023 with 0550/2770/0670 and apps cannot enter). The log names the winner.
 *
 * DEFAULT and ANDROID_WRITABLE (SAF provider com.android.externalstorage behind
 * the system "Files" app) are handled; NONE, INSTALLER and PASS_THROUGH are
 * skipped as raw /data/media consumers. No app list: every Zygote-started
 * process is covered. ANDROID_WRITABLE is the one mode number that is not the
 * same on every release — 8 on 11, 4 from 12 on — so both are accepted; see
 * is_android_writable().
 *
 * The libc entry patch (hook_libc.cpp) runs after specialisation, only on the
 * RAW source (on sdcardfs it is useless/harmful: the fs synthesises mode/group
 * and an ACL write cannot pass the mount). Disabled by
 * /data/adb/unfuse_zygisk.no_hooks, checked in preAppSpecialize while still
 * root, since /data/adb is invisible to apps.
 *
 * The tag gets one sample per boot, not one per app launch: the first app that
 * receives storage claims /data/adb/unfuse_zygisk.once and logs, the rest stay
 * quiet. post-fs-data.sh removes the marker at boot. Everything else on the tag
 * is an error. The sample names the Android release the hooks ran on, what the
 * loaded images declare about branch protection, and complains when the tally
 * differs from what that release is known to yield (android_ver.h).
 */

#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <sched.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/statfs.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <string>

#include <android/log.h>

#include "hook_libc.h"
#include "zygisk.hpp"

#define LOG_TAG "UnfuseZygisk"

namespace {

// Primary source: sdcardfs with full access (mask=0007, gid=9997).
constexpr const char *kSourceSdcardfs = "/mnt/runtime/full/emulated";

// Fallback: raw volume tree when sdcardfs is absent from the kernel.
constexpr const char *kSourceRaw = "/data/media";

constexpr const char *kNoHooksFlag = "/data/adb/unfuse_zygisk.no_hooks";

// Claimed by the first app that gets storage, so the tag carries one sample per
// boot instead of one per launch. post-fs-data.sh removes it.
constexpr const char *kOnceFlag = "/data/adb/unfuse_zygisk.once";

// Creates path if absent; true only for the caller that created it. Several
// apps specialise at once, so this must be atomic — O_EXCL is.
bool claim_once(const char *path) {
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    close(fd);
    return true;
}

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
// process the module declines, which the log names rather than hides.
constexpr int kMountModeExternalDefault = 1;
constexpr int kMountModeExternalAndroidWritable = 4;      // 12 and up
constexpr int kMountModeExternalAndroidWritableR = 8;     // 11 only

bool is_android_writable(int mode) {
    return mode == kMountModeExternalAndroidWritable ||
           mode == kMountModeExternalAndroidWritableR;
}

// AID_USER_OFFSET from android_filesystem_config.h
constexpr unsigned kAidUserOffset = 100000;

// SDCARDFS_SUPER_MAGIC, FUSE_SUPER_MAGIC (include/uapi/linux/magic.h)
constexpr unsigned long kSdcardFsMagic = 0x5dca2df5UL;
constexpr unsigned long kFuseMagic = 0x65735546UL;

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
// source itself, so the caller skips it when the source already is sdcardfs.
const char *runtime_view_for_mode(int mode) {
    if (mode == kMountModeExternalDefault) return "default";
    if (is_android_writable(mode)) return "full";
    return nullptr;
}

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

enum class Source { Sdcardfs, Raw };

void rollback(const std::string &dst) { umount2(dst.c_str(), MNT_DETACH); }

// Binds src onto dst; returns the fs type under the point. statfs() does not
// touch the inode, so unlike stat() it cannot be denied by directory permissions.
bool bind_and_type(const char *src, const std::string &dst, unsigned long *type) {
    if (mount(src, dst.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        LOGE("bind %s -> %s: %s", src, dst.c_str(), strerror(errno));
        return false;
    }

    struct statfs st {};
    if (statfs(dst.c_str(), &st) != 0) {
        LOGE("statfs %s: %s", dst.c_str(), strerror(errno));
        rollback(dst);
        return false;
    }

    *type = static_cast<unsigned long>(st.f_type);
    return true;
}

// Places a working source under dst. What matters is the fs type under the
// point, not mount(2) success: without sdcardfs storage.sh cannot mount it and
// the bind would expose an empty directory. On mismatch, roll back and try the
// raw tree, so the app never ends up with no storage.
bool attach(const std::string &dst, Source *used) {
    unsigned long type = 0;

    if (bind_and_type(kSourceSdcardfs, dst, &type)) {
        if (type == kSdcardFsMagic) {
            *used = Source::Sdcardfs;
            return true;
        }
        LOGE("%s: под точкой не sdcardfs (0x%lx) — откат", dst.c_str(), type);
        rollback(dst);
    }

    if (!bind_and_type(kSourceRaw, dst, &type)) return false;

    if (type == kFuseMagic) {
        LOGE("%s: под точкой остался FUSE (0x%lx) — откат", dst.c_str(), type);
        rollback(dst);
        return false;
    }

    *used = Source::Raw;
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
        if (unshare(CLONE_NEWNS) != 0) {
            LOGE("unshare(CLONE_NEWNS): %s", strerror(errno));
            return;
        }
        if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
            LOGE("MS_REC|MS_PRIVATE на /: %s", strerror(errno));
            return;
        }

        const std::string user =
            std::to_string(static_cast<unsigned>(args->uid) / kAidUserOffset);

        // Zygote takes whatever point it finds under /storage: usually
        // /mnt/user/<user>/emulated, or /mnt/androidwritable/<user>/emulated for
        // ANDROID_WRITABLE with persist.sys.vold_app_data_isolation_enabled.
        Source used = Source::Sdcardfs;
        bool ok = attach("/mnt/user/" + user + "/emulated", &used);

        if (android_writable) {
            Source writable = Source::Sdcardfs;
            if (attach("/mnt/androidwritable/" + user + "/emulated", &writable)) {
                ok = true;
                used = writable;
            }
        }

        // The other arm of Zygote's MountEmulatedStorage(): when it is going to
        // bind /mnt/runtime/<view> onto /storage instead of /mnt/user/<user>,
        // the substitution above is off the path. Put the source there as well.
        // Both binds are kept: /storage/self comes from /mnt/user/<user> even on
        // that arm, so dropping the first would lose it.
        std::string runtime_dst;
        if (zygote_uses_runtime_view()) {
            const char *view = runtime_view_for_mode(mode);
            if (view != nullptr) {
                const std::string dst =
                    std::string("/mnt/runtime/") + view + "/emulated";
                // ANDROID_WRITABLE's view is /mnt/runtime/full, which IS the
                // sdcardfs source: binding it onto itself adds nothing, while
                // the raw fallback still has to be substituted there.
                if (dst != kSourceSdcardfs) {
                    Source rt = Source::Sdcardfs;
                    if (attach(dst, &rt)) {
                        ok = true;
                        used = rt;
                        runtime_dst = dst;
                    }
                }
            }
        }
        if (!ok) return;

        if (args->mount_storage_dirs != nullptr) {
            *args->mount_storage_dirs = JNI_FALSE;
        }

        raw_ = (used == Source::Raw);
        hooks_allowed_ = (access(kNoHooksFlag, F_OK) != 0);

        // Both lines below are per-launch by nature, and a launch happens every
        // few seconds, so the tag is written only by the app that wins the
        // marker. status.sh reads that sample; when it is gone (logcat rolled
        // over) it falls back to the module being mapped.
        log_once_ = claim_once(kOnceFlag);
        if (log_once_) {
            // The arm is named only when the runtime view is the one in play, so
            // the sample says which half of MountEmulatedStorage() this release
            // actually ran — the one thing about the mount that is not the same
            // on every release.
            const std::string arm =
                runtime_dst.empty() ? std::string() : (", вид " + runtime_dst);
            LOGI("%s подключён: uid=%d%s%s",
                 used == Source::Sdcardfs ? "sdcardfs" : "сырой /data/media",
                 static_cast<int>(args->uid),
                 raw_ ? (hooks_allowed_ ? ", хуки включены" : ", хуки выключены файлом")
                      : "",
                 arm.c_str());
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (!raw_ || !hooks_allowed_) return;

        // Now specialised: this is app code, with its rights and namespace. The
        // libc patch cannot leak from here, unlike from preAppSpecialize.
        int total = 0;
        const int installed = hooks_install(&total);

        if (!log_once_) return;

        char report[768];
        hooks_report(report, sizeof report);

        // Which release this is, and what the tally is measured against
        // (android_ver.h). The version sits AFTER the target list: status.sh
        // keys on "хуки libc в uid=" and reads ok/alias out of that list, so a
        // prefix would be one more thing to keep out of its way.
        char ver[192];
        const int expected = hooks_release(ver, sizeof ver);

        // Branch protection of the images involved (hook_libc.cpp): invisible
        // until the day libc declares BTI, which is why it belongs in the sample.
        char bti[224];
        const int bti_state = hooks_bti_report(bti, sizeof bti);

        LOGI("хуки libc в uid=%d: %s | %s | %s",
             static_cast<int>(args->uid), report, ver, bti);

        if (bti_state < 0) {
            // The module's own note claims BTI while its handlers were not
            // compiled as landing pads: its pages would be guarded and the
            // branch into them unchecked. The patch is sound, the module is not.
            LOGE("модуль объявляет BTI, но собран без branch protection — "
                 "обработчики не площадки входа");
        } else if (bti_state > 0) {
            // Not an error: the patch opens with bti jc precisely so a guarded
            // entry stays a legal target. Said out loud because it changes the
            // threat model and would otherwise look like a mystery crash later.
            LOGI("libc объявила BTI — входы патча держатся на bti jc");
        }

        if (installed == 0) {
            LOGE("хуки libc не установлены ни одной цели — сырое дерево останется "
                 "без приведения режимов");
        } else if (expected > 0 && installed != expected) {
            // The release is in the table, so that number was measured on a real
            // image: a different one means the target list stopped covering this
            // build. Loud rather than fatal — the patch did install, just less.
            LOGE("хуки libc: покрыто %d целей, а %s ожидает %d — список целей "
                 "разошёлся с проверенным для этой сборки",
                 installed, ver, expected);
        }
    }

private:
    // State survives specialisation: both callbacks run on the same instance.
    bool raw_ = false;
    bool hooks_allowed_ = true;
    bool log_once_ = false;
};

}  // namespace

REGISTER_ZYGISK_MODULE(UnfuseZygisk)
