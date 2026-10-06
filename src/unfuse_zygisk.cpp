/*
 * Unfuse Zygisk — a Zygisk module restoring Android 10 internal-storage
 * behaviour: direct access to all storage for ALL apps, no FUSE, no
 * MediaProvider in the data path.
 *
 * (1) Places a storage source under /mnt/user/<user>/emulated in the process's
 * private mount namespace; Zygote then recursively binds it onto /storage, so
 * /storage/emulated is substituted too. (2) Clears *args->mount_storage_dirs so
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
 * process is covered.
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
 * is an error.
 */

#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <sched.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/statfs.h>
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

// android.os.storage.StorageManager.MOUNT_MODE_EXTERNAL_*
constexpr int kMountModeExternalDefault = 1;
constexpr int kMountModeExternalAndroidWritable = 4;

// AID_USER_OFFSET from android_filesystem_config.h
constexpr unsigned kAidUserOffset = 100000;

// SDCARDFS_SUPER_MAGIC, FUSE_SUPER_MAGIC (include/uapi/linux/magic.h)
constexpr unsigned long kSdcardFsMagic = 0x5dca2df5UL;
constexpr unsigned long kFuseMagic = 0x65735546UL;

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
        const bool android_writable = (mode == kMountModeExternalAndroidWritable);
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
            LOGI("%s подключён: uid=%d%s",
                 used == Source::Sdcardfs ? "sdcardfs" : "сырой /data/media",
                 static_cast<int>(args->uid),
                 raw_ ? (hooks_allowed_ ? ", хуки включены" : ", хуки выключены файлом")
                      : "");
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
        LOGI("хуки libc в uid=%d: %s", static_cast<int>(args->uid), report);

        if (installed == 0) {
            LOGE("хуки libc не установлены ни одной цели — сырое дерево останется "
                 "без приведения режимов");
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
