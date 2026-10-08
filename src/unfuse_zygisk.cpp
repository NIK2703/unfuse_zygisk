/*
 * Unfuse Zygisk — a Zygisk module restoring Android 10 internal-storage
 * behaviour: direct access to all storage for ALL apps, no FUSE, no
 * MediaProvider in the data path.
 *
 * The module mounts nothing, and that is the shape of this file. FUSE is not
 * defeated here — it is never created: tools/vold-fusefs redirects vold's
 * mount() trampoline so MountUserFuse() ends with a bind of the raw /data/media
 * tree on top of the FUSE mount it asked for. vold makes that bind on
 * /mnt/user/<user>/emulated, which is a shared mount, so it reaches every app
 * mount namespace by propagation, and Zygote then binds /mnt/user/<user> onto
 * /storage exactly as it always did.
 *
 * What is left for the module is one argument: *args->mount_storage_dirs is
 * cleared, so Zygote does not cover Android/{data,obb} with a per-package tmpfs,
 * giving full Android/data and Android/obb as before scoped storage. DEFAULT and
 * ANDROID_WRITABLE (the SAF provider com.android.externalstorage behind the
 * system "Files" app) are handled; NONE, INSTALLER and PASS_THROUGH are skipped
 * as raw /data/media consumers. No app list: every Zygote-started process is
 * covered. ANDROID_WRITABLE is the one mode number that is not the same on every
 * release — 8 on 11, 4 from 12 on — so both are accepted; see
 * is_android_writable().
 *
 * The libc entry patch (hook_libc.cpp) runs after specialisation, on the raw
 * source: it shapes modes into the sdcardfs view (group rw/rwx, other cleared)
 * and synthesises the group the raw tree does not carry. Disabled by
 * /data/adb/unfuse_zygisk.state/no_hooks, checked in preAppSpecialize while
 * still root, since /data/adb is invisible to apps.
 *
 * The tag gets one sample per boot, not one per app launch: the first app that
 * receives storage claims /data/adb/unfuse_zygisk.state/once and logs, the rest
 * stay quiet. post-fs-data.sh removes the marker at boot. Everything else on the
 * tag is an error. The sample names the domain this code runs in before
 * specialisation, the Android release the hooks ran on, what the loaded images
 * declare about branch protection, and complains when the tally differs from
 * what that release is known to yield (android_ver.h).
 *
 * ---------------------------------------------------------------------------
 * Why the state files live in a directory of their own — measured 2026-10-07
 * ---------------------------------------------------------------------------
 *
 * Both files used to sit directly in /data/adb, and neither of them ever did
 * anything: the tag stayed silent boot after boot, the sample never appeared
 * and the kill-switch could not be trusted either. The reason is the domain
 * this code runs in. preAppSpecialize is called before the process specialises,
 * so it is still the zygote domain, and the only thing the policy gives that
 * domain over /data/adb is traversal:
 *
 *   allow zygote adb_data_file dir search          (zygisksu/sepolicy.rule)
 *
 * search is what the path walk to the module's own .so needs, and it is all
 * there is. open(O_CREAT|O_EXCL) on /data/adb/unfuse_zygisk.once needs add_name
 * and write on the directory, so it fails — and the failure is dontaudit'ed, so
 * it shows up neither in logcat nor anywhere else. The module simply never
 * spoke, and nothing said why.
 *
 * Proved by running the device permissive for four seconds, which is the only
 * way to see a denial that is not audited: the moment SELinux stopped blocking,
 * the marker appeared as u:object_r:adb_data_file:s0 and both sample lines were
 * written; back in enforcing, both stopped again. No avc line either way.
 *
 * The fix is a directory labelled with the one type the policy hands out to
 * every domain — Zygisk Next's own rule, on the device at
 * /data/adb/modules/zygisksu/sepolicy.rule:
 *
 *   type magisk_file file_type
 *   typeattribute magisk_file mlstrustedobject
 *   allow * magisk_file dir *
 *   allow * magisk_file file *
 *
 * A file created inside such a directory inherits the label — verified on the
 * device, where touch and mkdir inside it both came out magisk_file — so the
 * module can create and write its state from the zygote domain without shipping
 * a policy rule of its own. post-fs-data.sh creates and labels the directory at
 * every boot, before Zygote starts, and re-labels it every time because nothing
 * guarantees the label survived the last boot.
 *
 * The switch fails open, deliberately. An absent no_hooks is the default, and a
 * state directory that cannot be read is not evidence that the user wanted the
 * hooks off: switching them off on that basis would turn a broken install into a
 * quiet no-op, which is exactly the failure just cured here. So an access() that
 * fails with anything other than ENOENT is reported rather than swallowed.
 *
 * ---------------------------------------------------------------------------
 * A second bind used to live here — removed 2026-10-07 as redundant
 * ---------------------------------------------------------------------------
 *
 * preAppSpecialize used to unshare(CLONE_NEWNS), make the whole tree private
 * (MS_REC|MS_PRIVATE on /) and then bind /data/media onto
 * /mnt/user/<user>/emulated itself, on top of vold's bind. That duplicated what
 * vold already does — and the pair was self-sustaining: the privatisation was
 * needed so the module's own bind would not leak into Zygote, and it in turn
 * severed the slave link through which vold's bind reaches the namespace, which
 * is what made the module's bind look necessary in the first place.
 *
 * Measured on the device (marble, Android 16 / sdk 36), straight out of
 * /proc/<pid>/mountinfo:
 *
 *   com.android.settings — never touched by the module. Root "/" is master:1,
 *   the ordinary slave copy Android's own unshare leaves behind, and
 *   /mnt/user/0/emulated carries master:49 (fuse) + master:39 (f2fs, vold's
 *   bind); /storage/emulated resolves to f2fs. vold's bind alone is enough.
 *
 *   com.termux — root "/" is private (the module's MS_REC|MS_PRIVATE), and
 *   /mnt/user/0/emulated carries TWO f2fs layers: vold's, inherited at unshare
 *   time, and the module's on top of it.
 *
 *   com.android.systemui — root "/" is private and /mnt/user/0/emulated has one
 *   f2fs layer and no FUSE mount at all: it specialised before vold mounted the
 *   volume, and the privatisation is exactly what kept vold's mount from
 *   propagating in. There the module's bind was the only layer — so removing
 *   the bind without also removing the privatisation would have dropped
 *   systemui and launcher3 onto nothing.
 *
 * Removing both restores the settings shape for every app: Android's per-app
 * namespace is a slave of the init root peer group, vold's emulated-storage
 * mounts are created shared, and a shared mount propagates to slaves as
 * master:<n>. Zygote's own MountEmulatedStorage() then needs no help.
 *
 * Consequence, stated rather than hidden: the arm that placed the raw tree at
 * /mnt/runtime/<view>/emulated went with the rest. It mattered only for the
 * pre-FUSE shape of Android 11, 12, 12L and 13 — persist.sys.fuse not true,
 * where Zygote binds /mnt/runtime/<view> onto /storage instead of
 * /mnt/user/<user>. MountUserFuse() only ever mounts under /mnt/user/<user>
 * (Utils.cpp:1600-1602), and /mnt/runtime/<view> is where SDCARDFS goes
 * (EmulatedVolume.cpp:360-363), so on such a release with sdcardfs disabled as
 * well nothing covers that path any more. On 14 and up — this device — the arm
 * never ran: persist.sys.fuse is true, so zygote_uses_runtime_view() was already
 * false.
 */

#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <android/log.h>

#include "hook_libc.h"
#include "zygisk.hpp"

#define LOG_TAG "UnfuseZygisk"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// The module's state directory: the two files below, and nothing else. Created
// and labelled by post-fs-data.sh before Zygote starts — the module cannot
// create it, and that is the whole point of the header above. A macro rather
// than a constant so the two paths stay compile-time joins of one name.
#define STATE_DIR "/data/adb/unfuse_zygisk.state"

// The switch: present means the libc patching stays off. Absent is the default.
constexpr const char *kNoHooksFlag = STATE_DIR "/no_hooks";

// Claimed by the first app that gets storage, so the tag carries one sample per
// boot instead of one per launch. post-fs-data.sh removes it.
constexpr const char *kOnceFlag = STATE_DIR "/once";

// Creates path if absent; true only for the caller that created it. Several
// apps specialise at once, so this must be atomic — O_EXCL is.
//
// EEXIST is the ordinary outcome for every app but the first, so it stays
// quiet. Anything else means the directory post-fs-data.sh prepares is missing
// or unreachable, and then the tag would go silent for the whole boot with no
// explanation — which is the defect this marker is recovering from. Say it out
// loud instead, on every launch, until it is fixed.
bool claim_once(const char *path) {
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (errno != EEXIST) {
            LOGE("маркер %s не создался: %s — сэмпла в логе не будет, "
                 "проверьте каталог состояния", path, strerror(errno));
        }
        return false;
    }
    close(fd);
    return true;
}

// The domain preAppSpecialize runs in. A field of its own because it is not
// knowable from outside: the process has not specialised yet, so /proc/<pid> of
// the running app shows the app's domain and never this one — and it is this
// one that decides whether the two paths above can be touched at all. "?" means
// the read was refused, which is itself an answer.
void current_domain(char *out, size_t n) {
    snprintf(out, n, "?");
    int fd = open("/proc/self/attr/current", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;

    const ssize_t got = read(fd, out, n - 1);
    close(fd);
    if (got <= 0) {
        snprintf(out, n, "?");
        return;
    }

    // The attribute comes back NUL-terminated, sometimes with a newline.
    size_t len = static_cast<size_t>(got);
    while (len > 0 && (out[len - 1] == '\0' || out[len - 1] == '\n')) out[--len] = '\0';
    if (len == 0) snprintf(out, n, "?");
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

class UnfuseZygisk : public zygisk::ModuleBase {
public:
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        const int mode = args->mount_external;
        if (mode != kMountModeExternalDefault && !is_android_writable(mode)) return;

        // The only thing this module asks of specialisation. Everything that
        // puts the raw tree under the app's storage is vold's business now: the
        // FUSE-off patch makes MountUserFuse() bind /data/media onto
        // /mnt/user/<user>/emulated, and the app namespace receives it by
        // propagation. See the header for why the module's own bind is gone.
        if (args->mount_storage_dirs != nullptr) {
            *args->mount_storage_dirs = JNI_FALSE;
        }

        // The switch is a file the user creates in the state directory; its
        // absence is the default and means "hooks on". See the header for why
        // this fails open and why a failure that is not plain "no such file" is
        // reported rather than swallowed.
        errno = 0;
        hooks_allowed_ = (access(kNoHooksFlag, F_OK) != 0);
        if (hooks_allowed_ && errno != 0 && errno != ENOENT) {
            LOGE("выключатель %s не прочитался: %s — хуки включены",
                 kNoHooksFlag, strerror(errno));
        }

        // Per-launch by nature, and a launch happens every few seconds, so the
        // tag is written only by the app that wins the marker. post-fs-data.sh
        // removes it before Zygote starts, so this is one line per boot.
        log_once_ = claim_once(kOnceFlag);
        if (log_once_) {
            char domain[64];
            current_domain(domain, sizeof domain);
            LOGI("Android/{data,obb} не изолируются, сырое дерево даёт bind vold: "
                 "uid=%d, домен %s%s",
                 static_cast<int>(args->uid), domain,
                 hooks_allowed_ ? ", хуки включены" : ", хуки выключены файлом");
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (!hooks_allowed_) return;

        // Now specialised: this is app code, with its rights and namespace. The
        // libc patch cannot leak from here, unlike from preAppSpecialize.
        // The attempts count is not wanted here — hookselftest.cpp is what reads it.
        const int installed = hooks_install(nullptr);

        if (!log_once_) return;

        char report[768];
        hooks_report(report, sizeof report);

        // Which release this is, and what the tally is measured against
        // (android_ver.h). The version sits AFTER the target list: scripts key
        // on "хуки libc в uid=" and read ok/alias out of that list, so a prefix
        // would be one more thing to keep out of their way.
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
    bool hooks_allowed_ = true;
    bool log_once_ = false;
};

}  // namespace

REGISTER_ZYGISK_MODULE(UnfuseZygisk)
