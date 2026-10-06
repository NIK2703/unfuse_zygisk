/*
 * sdcardfs-restore — Zygisk-модуль, возвращающий внутренней памяти поведение
 * Android 10 и более ранних версий: sdcardfs вместо FUSE и полный доступ ко всей
 * памяти для ВСЕХ приложений без исключений.
 *
 * Модуль делает ровно две вещи:
 *
 *   1. Подкладывает sdcardfs под /mnt/user/<user>/emulated в приватном mount
 *      namespace процесса. Дальше Zygote сам рекурсивно биндит эту точку на
 *      /storage, поэтому sdcardfs оказывается и на /storage/emulated.
 *
 *   2. Сбрасывает *args->mount_storage_dirs. Штатно Zygote накрывает
 *      /storage/emulated/<user>/Android/{data,obb} временным tmpfs и биндит туда
 *      каталоги ТОЛЬКО своего пакета; флаг отключает эту изоляцию, и Android/data
 *      с Android/obb отдаются целиком — как до scoped storage.
 *
 * Источник — /mnt/runtime/full/emulated: sdcardfs с маской 0007 и gid 9997
 * (AID_EVERYBODY), то есть каталоги 0770 и файлы 0660 для любого процесса.
 * Поднимает его storage.sh: на этой прошивке external_storage.sdcardfs.enabled=0,
 * поэтому vold sdcardfs не монтирует.
 *
 * Обрабатываются режимы DEFAULT (все обычные приложения) и ANDROID_WRITABLE
 * (нужен провайдеру SAF com.android.externalstorage, через который ходит
 * системное приложение «Файлы»). Пропускаются NONE, INSTALLER и PASS_THROUGH:
 * это не приложения, а потребители сырого /data/media, которым подмена сломала бы
 * работу самого хранилища.
 *
 * Списка приложений нет и быть не должно: подмена выполняется для каждого
 * процесса, который запускает Zygote.
 */

#include <errno.h>
#include <jni.h>
#include <sched.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/statfs.h>

#include <string>

#include <android/log.h>

#include "zygisk.hpp"

#define LOG_TAG "SdcardFsRestore"

namespace {

// Единственный источник: sdcardfs с полным доступом (mask=0007, gid=9997).
constexpr const char *kSource = "/mnt/runtime/full/emulated";

// android.os.storage.StorageManager.MOUNT_MODE_EXTERNAL_*
constexpr int kMountModeExternalDefault = 1;
constexpr int kMountModeExternalAndroidWritable = 4;

// AID_USER_OFFSET из android_filesystem_config.h
constexpr unsigned kAidUserOffset = 100000;

// SDCARDFS_SUPER_MAGIC из include/uapi/linux/magic.h
constexpr unsigned long kSdcardFsMagic = 0x5dca2df5UL;

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Подкладывает sdcardfs под dst. Результат подтверждается statfs(): если под
// точкой оказался не sdcardfs, маунт откатывается, чтобы приложение не осталось
// без памяти. statfs() не трогает inode, поэтому в отличие от stat() его нельзя
// отклонить из-за прав на сам каталог.
bool attach(const std::string &dst) {
    if (mount(kSource, dst.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        LOGE("bind %s -> %s: %s", kSource, dst.c_str(), strerror(errno));
        return false;
    }

    struct statfs st {};
    const bool ok = statfs(dst.c_str(), &st) == 0 &&
                    static_cast<unsigned long>(st.f_type) == kSdcardFsMagic;
    if (!ok) {
        LOGE("%s: под точкой не sdcardfs — откат", dst.c_str());
        umount2(dst.c_str(), MNT_DETACH);
    }
    return ok;
}

class SdcardFsRestore : public zygisk::ModuleBase {
public:
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        const int mode = args->mount_external;
        const bool android_writable = (mode == kMountModeExternalAndroidWritable);
        if (mode != kMountModeExternalDefault && !android_writable) return;

        // Приватная копия namespace: без неё маунт утечёт в zygote и во всех его
        // детей. unshare() копирует и настройки propagation, поэтому сразу
        // переводим всё дерево в private.
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

        // Zygote заберёт под /storage ту точку, которую найдёт: обычно
        // /mnt/user/<user>/emulated, а при включённом
        // persist.sys.vold_app_data_isolation_enabled для ANDROID_WRITABLE —
        // /mnt/androidwritable/<user>/emulated. Накрываем обе, какие есть.
        bool ok = attach("/mnt/user/" + user + "/emulated");
        if (android_writable && attach("/mnt/androidwritable/" + user + "/emulated")) {
            ok = true;
        }
        if (!ok) return;

        // Android/data и Android/obb отдаются целиком, как до scoped storage.
        if (args->mount_storage_dirs != nullptr) {
            *args->mount_storage_dirs = JNI_FALSE;
        }

        LOGI("sdcardfs подключён: uid=%d", static_cast<int>(args->uid));
    }
};

}  // namespace

REGISTER_ZYGISK_MODULE(SdcardFsRestore)
