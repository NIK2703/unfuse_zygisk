/*
 * sdcardfs-restore — Zygisk-модуль, возвращающий внутренней памяти поведение
 * Android 10 и более ранних версий: прямой доступ ко всей памяти для ВСЕХ
 * приложений без исключений, без FUSE и без MediaProvider на пути данных.
 *
 * Модуль делает ровно две вещи:
 *
 *   1. Подкладывает источник памяти под /mnt/user/<user>/emulated в приватном
 *      mount namespace процесса. Дальше Zygote сам рекурсивно биндит эту точку
 *      на /storage, поэтому подмена оказывается и на /storage/emulated.
 *
 *   2. Сбрасывает *args->mount_storage_dirs. Штатно Zygote накрывает
 *      /storage/emulated/<user>/Android/{data,obb} временным tmpfs и биндит туда
 *      каталоги ТОЛЬКО своего пакета; флаг отключает эту изоляцию, и Android/data
 *      с Android/obb отдаются целиком — как до scoped storage.
 *
 * Источник выбирается на месте, по факту, а не по конфигу:
 *
 *   - основной — /mnt/runtime/full/emulated, sdcardfs с маской 0007 и gid 9997
 *     (AID_EVERYBODY): каталоги 0770 и файлы 0660 для любого процесса. Его
 *     поднимает storage.sh: на этой прошивке external_storage.sdcardfs.enabled=0,
 *     поэтому vold sdcardfs не монтирует;
 *
 *   - альтернативный — сырой /data/media, если ядро собрано без sdcardfs. Это
 *     ровно тот же путь, который AOSP отдаёт приложениям на /mnt/pass_through
 *     (vold-16/Utils.cpp:1691 биндит туда absolute_lower_path). Права на дерево
 *     под этот путь расставляет storage.sh: ACL с именованной записью для группы
 *     9997 (AID_EVERYBODY) — той самой, которой пользуются sdcardfs-маунты
 *     read/write/full. Без этой расстановки дерево принадлежит 1023:1023 с
 *     режимами 0550/2770/0670, и приложения в него не войдут.
 *
 * Какой путь сработал — видно в логе: "sdcardfs подключён" либо
 * "сырой /data/media подключён".
 *
 * Обрабатываются режимы DEFAULT (все обычные приложения) и ANDROID_WRITABLE
 * (нужен провайдеру SAF com.android.externalstorage, через который ходит
 * системное приложение «Файлы»). Пропускаются NONE, INSTALLER и PASS_THROUGH:
 * это не приложения, а потребители сырого /data/media, которым подмена сломала бы
 * работу самого хранилища.
 *
 * Списка приложений нет и быть не должно: подмена выполняется для каждого
 * процесса, который запускает Zygote.
 *
 * Отдельно, уже после специализации процесса, включается правка входов libc
 * (hook_libc.cpp) — но только если сработал СЫРОЙ источник. На sdcardfs она не
 * нужна и вредна: там режимы и группу синтезирует сама файловая система, а
 * запись ACL в sdcardfs-маунт не пройдёт. Отключается файлом
 * /data/adb/sdcardfs_restore.no_hooks — он проверяется в preAppSpecialize, пока
 * процесс ещё root: /data/adb недоступен приложениям.
 */

#include <errno.h>
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

#define LOG_TAG "SdcardFsRestore"

namespace {

// Основной источник: sdcardfs с полным доступом (mask=0007, gid=9997).
constexpr const char *kSourceSdcardfs = "/mnt/runtime/full/emulated";

// Альтернативный источник: сырое дерево тома, когда sdcardfs в ядре нет.
constexpr const char *kSourceRaw = "/data/media";

// Файл-выключатель правки входов libc. Проверяется в preAppSpecialize, пока
// процесс ещё root: из приложения /data/adb не виден.
constexpr const char *kNoHooksFlag = "/data/adb/sdcardfs_restore.no_hooks";

// android.os.storage.StorageManager.MOUNT_MODE_EXTERNAL_*
constexpr int kMountModeExternalDefault = 1;
constexpr int kMountModeExternalAndroidWritable = 4;

// AID_USER_OFFSET из android_filesystem_config.h
constexpr unsigned kAidUserOffset = 100000;

// SDCARDFS_SUPER_MAGIC и FUSE_SUPER_MAGIC из include/uapi/linux/magic.h
constexpr unsigned long kSdcardFsMagic = 0x5dca2df5UL;
constexpr unsigned long kFuseMagic = 0x65735546UL;

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

enum class Source { Sdcardfs, Raw };

void rollback(const std::string &dst) { umount2(dst.c_str(), MNT_DETACH); }

// Биндит src на dst и возвращает тип ФС, оказавшейся под точкой. statfs()
// не трогает inode, поэтому в отличие от stat() его нельзя отклонить из-за прав
// на сам каталог.
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

// Подкладывает рабочий источник памяти под dst. Проверяется не факт успеха
// mount(2), а тип ФС под точкой: без sdcardfs в ядре storage.sh смонтировать его
// не может, и bind подсунул бы пустой каталог. Поэтому при несовпадении маунт
// откатывается, и пробуется сырое дерево. Такой же откат нужен, чтобы приложение
// не осталось вообще без памяти.
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

        // Android/data и Android/obb отдаются целиком, как до scoped storage.
        if (args->mount_storage_dirs != nullptr) {
            *args->mount_storage_dirs = JNI_FALSE;
        }

        // Правка входов libc нужна только на сыром дереве: на sdcardfs режимы и
        // группу синтезирует файловая система, и трогать их не надо.
        raw_ = (used == Source::Raw);

        // Выключатель читается здесь, пока процесс ещё root: /data/adb закрыт
        // для приложений, и в postAppSpecialize его было бы не видно.
        hooks_allowed_ = (access(kNoHooksFlag, F_OK) != 0);

        LOGI("%s подключён: uid=%d%s",
             used == Source::Sdcardfs ? "sdcardfs" : "сырой /data/media",
             static_cast<int>(args->uid),
             raw_ ? (hooks_allowed_ ? ", хуки включены" : ", хуки выключены файлом")
                  : "");
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (!raw_ || !hooks_allowed_) return;

        // Процесс уже специализирован: это код приложения, с его правами и в его
        // namespace. Правка входов libc здесь никуда не утечёт — в отличие от
        // preAppSpecialize, откуда её забрал бы с собой zygote.
        int total = 0;
        const int installed = hooks_install(&total);

        char report[768];
        hooks_report(report, sizeof report);
        LOGI("хуки libc в uid=%d: %s", static_cast<int>(args->uid), report);

        if (installed == 0) {
            LOGE("хуки libc не установлены ни одной цели — сырое дерево останется "
                 "без приведения режимов");
        }
    }

private:
    // Состояние переживает специализацию: preAppSpecialize и postAppSpecialize
    // вызываются в одном и том же процессе над одним и тем же экземпляром.
    bool raw_ = false;
    bool hooks_allowed_ = true;
};

}  // namespace

REGISTER_ZYGISK_MODULE(SdcardFsRestore)
