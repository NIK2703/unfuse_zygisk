/*
 * sdcardfs-restore — Zygisk-модуль, возвращающий внутренней памяти поведение
 * Android 10 и более ранних версий: sdcardfs вместо FUSE, полный доступ ко всем
 * каталогам, без scoped storage и без изоляции Android/data и Android/obb.
 *
 * МОДУЛЬ НЕ ЗНАЕТ ПОНЯТИЯ «СПИСОК ПРИЛОЖЕНИЙ»: подмена выполняется для КАЖДОГО
 * процесса, который запускает Zygote с mount_external == DEFAULT или
 * ANDROID_WRITABLE, то есть для всех приложений.
 *
 * ANDROID_WRITABLE нужен отдельно: с ним работают приложения с
 * MANAGE_EXTERNAL_STORAGE и системный провайдер SAF com.android.externalstorage,
 * через который ходит DocumentsUI. Пока провайдер оставался на FUSE, «Файлы»
 * показывали Android/data и Android/obb пустыми: поверх FUSE там лежит
 * mirror-маунт сырой ФС (/data/media/0/Android/data, режим 2771 uid/gid 1023),
 * листинг которого провайдеру запрещён по DAC — остаётся только --x.
 *
 * Пропускает модуль только настоящих потребителей сырого /data/media:
 *   NONE         — изолированные процессы (внешнего хранилища нет вовсе);
 *   INSTALLER    — installd;
 *   PASS_THROUGH — MediaProvider.
 * Если подменить хранилище им, отвалится сам слой хранилища.
 *
 * Вариант B из aosp-ref/DESIGN-zygisk-sdcardfs.md.
 *
 * Как это работает:
 *   preAppSpecialize() вызывается Zygisk в уже форкнутом ребёнке Zygote —
 *   uid 0, домен u:r:zygote:s0, до того как Zygote выполнит
 *   BindMount("/mnt/user/<u>", "/storage", MS_BIND|MS_REC). Модуль создаёт
 *   приватный mount namespace и подкладывает sdcardfs под
 *   /mnt/user/<user>/emulated вместо FUSE. Рекурсивный бинд Zygote сам
 *   протаскивает sdcardfs на /storage/emulated.
 *
 * Два независимых пути подмены:
 *   1) bind уже смонтированного sdcardfs (/mnt/runtime/full/emulated) —
 *      дёшево: superblock общий, новый inode-кэш не создаётся;
 *   2) если источника не видно — модуль монтирует sdcardfs сам, прямо на
 *      /mnt/user/<u>/emulated. Ни от чего не зависит.
 * Успех каждого пути подтверждается statfs() по цели: если под точкой оказался
 * не sdcardfs (магия 0x5dca2df5), маунт немедленно откатывается. Поэтому
 * подсунуть приложению пустую заглушку вместо памяти невозможно.
 *
 * Что действительно мешает (проверено на устройстве) — две РАЗНЫЕ причины:
 *
 * (1) Права DAC — почему падал сам маунт.
 *   В момент preAppSpecialize процесс ещё не root в полном смысле: /mnt/runtime
 *   имеет режим 0700 root:root, /data/media — 0550 uid 1023 (media_rw). Из-за
 *   этого предварительный stat() по источнику бинда отдавал EACCES, и в логе
 *   НЕ было AVC — отказ чисто файловый. Лечится тем, что модуль больше НЕ
 *   делает stat() по источнику, а проверяет результат через statfs() по цели;
 *   дополнительно post-fs-data/service снимают лишний бит с /mnt/runtime и
 *   /data/media (см. !relax= в config). Сам маунт пробуется и под подменённой
 *   личностью (fsuid=1023 media_rw, fsgid=9997 everybody) — это ровно
 *   владельцы /data/media и /mnt/user/0.
 *
 * (2) SELinux — почему отваливался КОРЕНЬ хранилища.
 *   sdcardfs не заводит собственный инод для корня точки монтирования:
 *   getattr() корня форвардится в НИЖНИЙ инод, то есть в /data/media. Ярлык
 *   корня — media_userdir_file, и для него coredomain/appdomain имеют ровно
 *   одно право — search (domain.te:252). Отсюда
 *       stat /storage/emulated    -> EACCES   (снова без единого AVC: dontaudit)
 *       stat /storage/emulated/0  -> ok
 *   Содержимое работает, потому что /data/media/.* помечен media_rw_data_file,
 *   а на него у appdomain полные права на каталог (app.te:149 create_dir_perms).
 *   Решение — вернуть корню ярлык media_rw_data_file (module/relabel-media.sh,
 *   директива !relabel=), ровно как было на Android 10 и раньше. Патчить
 *   sepolicy не нужно и нельзя: право уже есть, а relabelto для zygote
 *   запрещён neverallow (domain.te:791) — поэтому перемаркировка делается
 *   скриптом модуля, а не из этого процесса.
 *
 * Проверки SELinux для этого сценария как раз разрешающие (AOSP 16):
 *   - unshare(CLONE_NEWNS)                     — только capable(sys_admin)
 *   - mount(NULL,"/",MS_REC|MS_PRIVATE)        — zygote.te: rootfs:dir mounton
 *   - mounton цели /mnt/user/<u>/emulated      — zygote.te: { sdcard_type fuse }:dir mounton
 *   - search /data/media для прямого маунта    — domain.te: { coredomain appdomain }
 *   - доступ приложений к содержимому          — app.te: { sdcard_type fuse }:dir|file create_*_perms
 *   - /storage после бинда = mnt_user_file     — app.te: mnt_user_file:dir r_dir_perms
 *
 * Почему приложениям подходит маска 0007 и gid 9997:
 *   драйвер считает visible_mode = 0775 & ~mask, а gid берёт из опции gid=.
 *   mask=7 (oct 0007) + gid=9997 (AID_EVERYBODY) дают 0770 на каталоги и 0660 на
 *   файлы для ВСЕХ процессов, потому что AID_EVERYBODY есть у любого процесса
 *   (ProcessList.computeGidsForProcess всегда добавляет userGid).
 *   Ровно так же вёл себя sdcardfs-маунт "full" на Android 9 и раньше.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <stdlib.h>
#include <vector>

// bionic отдаёт их только через <sys/fsuid.h>, которого нет в NDK-заголовках.
extern "C" int setfsuid(uid_t fsuid);
extern "C" int setfsgid(gid_t fsgid);

#include <android/log.h>

#include "zygisk.hpp"

// bionic может не отдать CLONE_NEWNS через <sched.h>
#ifndef CLONE_NEWNS
#define CLONE_NEWNS 0x00020000
#endif

#define LOG_TAG "SdcardFsRestore"

namespace {

constexpr const char *kTag = LOG_TAG;

// ---------------------------------------------------------------- константы

// Конфиг лежит в каталоге модуля, а не в /data/adb: сам /data/adb/*.conf помечен
// adb_data_file, а домен zygote не имеет права читать data_file_type (neverallow
// в AOSP zygote.te). Каталог модуля перемаркирован в system_file и читается.
constexpr const char *kConfigPath =
    "/data/adb/modules/sdcardfs_restore/config";

constexpr const char *kDisableMarkers[] = {
    "/data/adb/sdcardfs_restore.disable",
    "/data/adb/modules/sdcardfs_restore/disable",
};

constexpr const char *kVerboseMarker =
    "/data/adb/modules/sdcardfs_restore/verbose";

// Источники для дешёвого bind. Первый подходящий выигрывает.
// Порядок: full → write (у обоих gid 9997 и маска 0007, то есть полный доступ).
constexpr const char *kBindSources[] = {
    "/mnt/runtime/full/emulated",
    "/mnt/runtime/write/emulated",
};

// Опции sdcardfs-маунта — те же, что vold передаёт /system/bin/sdcard, плюс
// mask=7 (десятичное, т.е. oct 0007) и gid=9997 (AID_EVERYBODY): полный доступ
// ко всей внутренней памяти для всех процессов, как на Android 10 и раньше.
// userid= дописывается значением на месте.
constexpr const char *kSdcardFsOptions =
    "fsuid=1023,fsgid=1023,multiuser,derive_gid,default_normal,unshared_obb,"
    "mask=7,gid=9997,userid=";

// Нижний слой, из которого sdcardfs берёт данные.
constexpr const char *kLowerRoot = "/data/media";

// android.os.storage.StorageManager.MOUNT_MODE_EXTERNAL_*
// Значения — как в A16 (в Android 11 их было 9, в A16 осталось 5):
//   NONE = 0, DEFAULT = 1, INSTALLER = 2, PASS_THROUGH = 3, ANDROID_WRITABLE = 4
constexpr int kMountModeExternalNone = 0;
constexpr int kMountModeExternalDefault = 1;
constexpr int kMountModeExternalAndroidWritable = 4;

// Только для читаемых логов.
const char *mountModeName(int mode) {
    switch (mode) {
        case kMountModeExternalNone: return "NONE/изолированный";
        case kMountModeExternalDefault: return "DEFAULT";
        case 2: return "INSTALLER";
        case 3: return "PASS_THROUGH";
        case 4: return "ANDROID_WRITABLE";
        default: return "?";
    }
}

// AID_USER_OFFSET из android_filesystem_config.h
constexpr unsigned kAidUserOffset = 100000;

// Владельцы нижнего слоя: /data/media — 0550 uid AID_MEDIA_RW,
// /mnt/user — 0750 gid AID_MEDIA_RW, /mnt/user/0 — 0710 gid AID_EVERYBODY.
constexpr uid_t kAidMediaRw = 1023;
constexpr gid_t kAidEverybody = 9997;

// SDCARDFS_SUPER_MAGIC из include/uapi/linux/magic.h
constexpr unsigned long kSdcardFsMagic = 0x5dca2df5UL;

constexpr size_t kMaxConfigBytes = 64 * 1024;

bool g_verbose = false;

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kTag, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, kTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kTag, __VA_ARGS__)
#define LOGV(...)                                                        \
    do {                                                                 \
        if (g_verbose) {                                                 \
            __android_log_print(ANDROID_LOG_DEBUG, kTag, __VA_ARGS__);   \
        }                                                                \
    } while (0)

// ---------------------------------------------------------------- утилиты

bool readWholeFile(const char *path, std::string &out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    out.clear();
    char buf[4096];
    bool truncated = false;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        if (out.size() + static_cast<size_t>(n) > kMaxConfigBytes) {
            out.append(buf, kMaxConfigBytes - out.size());
            truncated = true;
            break;
        }
        out.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    if (truncated) LOGW("файл %s обрезан до %zu байт", path, kMaxConfigBytes);
    return true;
}

std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool fileExists(const char *p) { return access(p, F_OK) == 0; }

bool disableMarkerExists() {
    for (const char *p : kDisableMarkers) {
        if (fileExists(p)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- конфиг

// Конфиг содержит ТОЛЬКО глобальные переключатели. Списка пакетов здесь нет и
// быть не должно: модуль обязан работать для всех приложений без исключений.
struct Config {
    bool enabled = true;
    bool raw_android_dirs = true;
    bool selftest = false;
};

Config parseConfig(const std::string &raw) {
    Config cfg;
    size_t pos = 0;
    while (pos <= raw.size()) {
        size_t nl = raw.find('\n', pos);
        std::string line =
            raw.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? raw.size() + 1 : nl + 1;

        line = trim(line);
        if (line.empty() || line[0] != '!') continue;

        const std::string d = trim(line.substr(1));
        if (d == "enabled=0" || d == "enabled=false") {
            cfg.enabled = false;
        } else if (d == "enabled=1" || d == "enabled=true") {
            cfg.enabled = true;
        } else if (d == "verbose=1" || d == "verbose=true") {
            g_verbose = true;
        } else if (d == "verbose=0" || d == "verbose=false") {
            g_verbose = false;
        } else if (d == "android_dirs=raw" || d == "android_dirs=1" ||
                   d == "android_dirs=true") {
            cfg.raw_android_dirs = true;
        } else if (d == "android_dirs=isolated" || d == "android_dirs=0" ||
                   d == "android_dirs=false") {
            cfg.raw_android_dirs = false;
        } else if (d == "selftest=1" || d == "selftest=true") {
            cfg.selftest = true;
        } else if (d == "selftest=0" || d == "selftest=false") {
            cfg.selftest = false;
        } else if (d.rfind("relax=", 0) == 0) {
            // Директива принадлежит relax-storage.sh: сам модуль права не
            // меняет, поэтому просто не считаем её опечаткой.
        } else if (d.rfind("relabel=", 0) == 0) {
            // Директива принадлежит relabel-media.sh: модуль ярлыки не меняет
            // (relabelto для домена zygote запрещён neverallow), поэтому тоже
            // просто не считаем её опечаткой.
        } else {
            LOGW("неизвестная директива: !%s", d.c_str());
        }
    }
    return cfg;
}

struct State {
    bool loaded = false;
    Config cfg;
};

State g_state;

void ensureConfig() {
    if (g_state.loaded) return;
    g_state.loaded = true;

    if (fileExists(kVerboseMarker)) g_verbose = true;

    std::string raw;
    if (readWholeFile(kConfigPath, raw)) {
        g_state.cfg = parseConfig(raw);
        LOGI("конфиг: %s (enabled=%d, verbose=%d)", kConfigPath,
             g_state.cfg.enabled ? 1 : 0, g_verbose ? 1 : 0);
    } else {
        LOGI("конфига нет (%s) — работаем по умолчанию: включено, все приложения",
             kConfigPath);
    }

    if (disableMarkerExists()) {
        g_state.cfg.enabled = false;
        LOGW("найден файл-маркер отключения — модуль выключен");
    }
}

// ---------------------------------------------------------------- диагностика


bool fsMagic(const char *path, unsigned long *out) {
    struct statfs st {};
    if (statfs(path, &st) != 0) return false;
    if (out != nullptr) *out = static_cast<unsigned long>(st.f_type);
    return true;
}

std::string hexOf(unsigned long v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%lx", v);
    return std::string(buf);
}

// Кто мы на самом деле в момент preAppSpecialize. От этого зависит всё: без
// CAP_DAC_OVERRIDE каталог /mnt/runtime (0700 root:root) недостижим, и отказ
// будет чисто файловым (EACCES без единого AVC в логе).
void logIdentity() {
    if (!g_verbose) return;

    LOGI("ид: uid=%d euid=%d gid=%d egid=%d", static_cast<int>(getuid()),
         static_cast<int>(geteuid()), static_cast<int>(getgid()),
         static_cast<int>(getegid()));

    gid_t groups[64];
    const int n = getgroups(64, groups);
    if (n >= 0) {
        std::string s;
        for (int i = 0; i < n; ++i) {
            s += std::to_string(static_cast<int>(groups[i]));
            if (i + 1 < n) s += ',';
        }
        LOGI("ид: groups(%d)=%s", n, s.c_str());
    } else {
        LOGI("ид: groups -> %s", strerror(errno));
    }

    std::string st;
    if (readWholeFile("/proc/self/status", st)) {
        const char *keys[] = {"CapInh:", "CapPrm:", "CapEff:", "CapBnd:",
                              "NoNewPrivs:", "Seccomp:"};
        for (const char *k : keys) {
            const size_t p = st.find(k);
            if (p == std::string::npos) continue;
            size_t e = st.find('\n', p);
            if (e == std::string::npos) e = st.size();
            LOGI("ид: %s", st.substr(p, e - p).c_str());
        }
    }

    std::string m;
    if (readWholeFile("/proc/self/uid_map", m)) {
        for (char &c : m) {
            if (c == '\n') c = ' ';
        }
        LOGI("ид: uid_map=[%s]", m.c_str());
    }
    m.clear();
    if (readWholeFile("/proc/self/gid_map", m)) {
        for (char &c : m) {
            if (c == '\n') c = ' ';
        }
        LOGI("ид: gid_map=[%s]", m.c_str());
    }
}

// Пошагово: на каком именно компоненте пути отказывает stat и что показывает
// statfs (statfs не смотрит на inode, поэтому он же используется для проверки
// результата маунта).
void logPathLadder() {
    if (!g_verbose) return;

    const char *paths[] = {
        "/", "/mnt", "/mnt/runtime", "/mnt/runtime/full",
        "/mnt/runtime/full/emulated", "/mnt/runtime/full/emulated/0",
        "/mnt/runtime/write/emulated", "/mnt/user", "/mnt/user/0",
        "/mnt/user/0/emulated", "/data", "/data/media", "/data/media/0",
    };

    for (const char *p : paths) {
        struct stat sb {};
        errno = 0;
        const int rc = stat(p, &sb);
        const std::string st = (rc == 0) ? "ok" : strerror(errno);

        unsigned long magic = 0;
        errno = 0;
        const bool okfs = fsMagic(p, &magic);

        LOGI("ид: %-32s stat=%-18s statfs=%s", p, st.c_str(),
             okfs ? hexOf(magic).c_str() : strerror(errno));
    }
}

// Что именно не даёт смонтировать. Все пробы откатываются сразу после успеха,
// чтобы не сломать приложению хранилище.
void runMountProbes(const std::string &dst, const std::string &opts) {
    if (!g_verbose) return;

    auto probe = [&dst](const char *what, const char *src, const char *fstype,
                        unsigned long flags, const char *data) {
        errno = 0;
        if (mount(src, dst.c_str(), fstype, flags, data) == 0) {
            LOGI("проба: %s -> УСПЕХ", what);
            umount2(dst.c_str(), MNT_DETACH);
            return;
        }
        LOGI("проба: %s -> %s", what, strerror(errno));
    };

    probe("tmpfs на цель (контроль mounton)", "tmpfs", "tmpfs", 0, "mode=0777");
    probe("bind цель->цель (контроль)", dst.c_str(), nullptr, MS_BIND, nullptr);
    probe("bind full/emulated -> цель (без REC)", kBindSources[0], nullptr, MS_BIND,
          nullptr);
    probe("bind full/emulated -> цель (MS_REC)", kBindSources[0], nullptr,
          MS_BIND | MS_REC, nullptr);
    probe("bind write/emulated -> цель (MS_REC)", kBindSources[1], nullptr,
          MS_BIND | MS_REC, nullptr);
    probe("sdcardfs /data/media -> цель", kLowerRoot, "sdcardfs", 0, opts.c_str());
}

// ---------------------------------------------------------------- SELinux-диагностика

// libselinux нет в NDK, поэтому берём её с устройства. Нужна ровно затем, чтобы
// при отказе получить не «Permission denied», а точную пару
// (домен, метка, класс, право). Отказы в этой прошивке частично под dontaudit,
// поэтому в logcat AVC не появляется и угадывать бессмысленно.
void freecon_(void *h, char *ctx);

void dumpSelinuxDiagnostics(const std::string &dst) {
    if (!g_verbose) return;

    void *h = dlopen("libselinux.so", RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
        LOGW("диагностика: libselinux недоступна (%s)", dlerror());
        return;
    }

    auto p_getcon = reinterpret_cast<int (*)(char **)>(dlsym(h, "getcon"));
    auto p_getfilecon =
        reinterpret_cast<int (*)(const char *, char **)>(dlsym(h, "getfilecon"));
    auto p_check = reinterpret_cast<int (*)(const char *, const char *, const char *,
                                            const char *, void *)>(
        dlsym(h, "selinux_check_access"));

    if (p_getcon == nullptr || p_check == nullptr) {
        LOGW("диагностика: в libselinux нет нужных символов");
        dlclose(h);
        return;
    }

    char *scon = nullptr;
    if (p_getcon(&scon) != 0 || scon == nullptr) {
        LOGW("диагностика: getcon не сработал");
        dlclose(h);
        return;
    }
    LOGI("диагностика: наш домен = %s", scon);

    const char *paths[] = {"/mnt/runtime", "/mnt/runtime/full",
                           "/mnt/runtime/full/emulated", dst.c_str(),
                           "/data/media", "/storage"};
    for (const char *p : paths) {
        char *fcon = nullptr;
        if (p_getfilecon != nullptr && p_getfilecon(p, &fcon) == 0 && fcon != nullptr) {
            LOGI("диагностика: %s -> %s", p, fcon);
            freecon_(h, fcon);
        } else {
            LOGI("диагностика: %s -> (метку не получить)", p);
        }
    }

    struct Probe {
        const char *path;
        const char *tclass;
        const char *perm;
    };
    // ВНИМАНИЕ: для корня sdcardfs (/mnt/runtime/full/emulated) эта проба
    // СОВРЁТ «РАЗРЕШЕНО», потому что getfilecon() отдаёт метку точки
    // монтирования (sdcardfs), а ядро проверяет ещё и метку НИЖНЕГО инода
    // (/data/media = media_userdir_file). Поэтому решает не эта проба, а
    // строка "-> %s" с меткой /data/media выше: она должна показывать
    // media_rw_data_file, иначе stat()/ls() корня хранилища будут EACCES.
    const Probe probes[] = {
        {"/mnt/runtime", "dir", "search"},
        {"/mnt/runtime/full", "dir", "search"},
        {"/mnt/runtime/full/emulated", "dir", "getattr"},
        {"/mnt/runtime/full/emulated", "dir", "search"},
        {"/mnt/runtime/full/emulated", "dir", "mounton"},
        {"/mnt/runtime/full/emulated/0", "dir", "getattr"},
        {"/mnt/user/0", "dir", "search"},
        {"/mnt/user/0/emulated", "dir", "search"},
        {"/mnt/user/0/emulated", "dir", "getattr"},
        {"/mnt/user/0/emulated", "dir", "mounton"},
        {"/data/media", "dir", "search"},
        {"/data/media", "dir", "getattr"},
        {"/data/media/0", "dir", "search"},
        {"/data/media/0", "dir", "getattr"},
        {"/", "dir", "mounton"},
    };

    for (const Probe &pr : probes) {
        char *fcon = nullptr;
        if (p_getfilecon == nullptr || p_getfilecon(pr.path, &fcon) != 0 || fcon == nullptr) {
            LOGI("диагностика: %s %s:%s -> метка недоступна", pr.path, pr.tclass, pr.perm);
            continue;
        }
        errno = 0;
        const int rc = p_check(scon, fcon, pr.tclass, pr.perm, nullptr);
        LOGI("диагностика: allow %s %s:%s %s -> %s", scon, fcon, pr.tclass, pr.perm,
             rc == 0 ? "РАЗРЕШЕНО" : "ОТКАЗАНО");
        freecon_(h, fcon);
    }

    freecon_(h, scon);
    dlclose(h);
}

// freecon() объявлена в libselinux; берём оттуда же.
void freecon_(void *h, char *ctx) {
    if (ctx == nullptr) return;
    auto p_freecon = reinterpret_cast<int (*)(char *)>(dlsym(h, "freecon"));
    if (p_freecon != nullptr) {
        p_freecon(ctx);
    } else {
        free(ctx);
    }
}

// ---------------------------------------------------------------- подмена

// Одна попытка подложить sdcardfs под dst: сначала bind уже готового маунта,
// затем собственный маунт драйвера. Каждый успешный маунт подтверждается
// statfs по цели — иначе откат, чтобы приложение не осталось с пустой
// заглушкой вместо внутренней памяти.
bool tryAttach(const std::string &dst, unsigned user_id, std::string &detail) {
    // --- Путь 1: bind уже смонтированного sdcardfs. Дёшево: superblock общий.
    for (const char *cand : kBindSources) {
        errno = 0;
        if (mount(cand, dst.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
            LOGV("bind %s -> %s: %s", cand, dst.c_str(), strerror(errno));
            continue;
        }
        unsigned long magic = 0;
        if (fsMagic(dst.c_str(), &magic) && magic == kSdcardFsMagic) {
            detail = std::string("bind ") + cand;
            return true;
        }
        LOGW("bind %s прошёл, но %s отдаёт %s — откат", cand, dst.c_str(),
             hexOf(magic).c_str());
        umount2(dst.c_str(), MNT_DETACH);
    }

    // --- Путь 2: монтируем sdcardfs сами. Ни от чего не зависим.
    const std::string opts =
        std::string(kSdcardFsOptions) + std::to_string(user_id);
    errno = 0;
    if (mount(kLowerRoot, dst.c_str(), "sdcardfs", 0, opts.c_str()) == 0) {
        unsigned long magic = 0;
        if (fsMagic(dst.c_str(), &magic) && magic == kSdcardFsMagic) {
            detail = std::string("mount -t sdcardfs ") + kLowerRoot;
            return true;
        }
        LOGW("маунт sdcardfs прошёл, но %s отдаёт %s — откат", dst.c_str(),
             hexOf(magic).c_str());
        umount2(dst.c_str(), MNT_DETACH);
    } else {
        LOGV("прямой маунт sdcardfs -> %s: %s", dst.c_str(), strerror(errno));
    }

    return false;
}

// Делает dst точкой sdcardfs в ТЕКУЩЕМ (приватном) namespace.
// Возвращает true при успехе, заполняя detail.
//
// dst — это ровно тот каталог, который Zygote затем рекурсивно биндит на
// /storage (см. MountEmulatedStorage в com_android_internal_os_Zygote.cpp):
//   /mnt/user/<user>/emulated             — DEFAULT и обычно ANDROID_WRITABLE
//   /mnt/androidwritable/<user>/emulated  — ANDROID_WRITABLE, когда включён
//                                           persist.sys.vold_app_data_isolation_enabled
bool attachSdcardFs(unsigned user_id, const std::string &dst, std::string &detail) {
    // Проверяем именно statfs: он не трогает inode и не может быть отклонён
    // из-за прав на сам каталог (в отличие от прежнего stat()).
    if (!fsMagic(dst.c_str(), nullptr)) {
        detail = "нет точки " + dst + ": " + strerror(errno);
        return false;
    }

    // Приватная копия namespace: без неё маунт утечёт в zygote и во всех его
    // детей. unshare() копирует и настройки propagation, поэтому сразу
    // переводим всё дерево в private — иначе маунт всё равно уползёт обратно.
    if (unshare(CLONE_NEWNS) != 0) {
        detail = std::string("unshare(CLONE_NEWNS): ") + strerror(errno);
        return false;
    }
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        detail = std::string("MS_REC|MS_PRIVATE на /: ") + strerror(errno);
        return false;
    }

    logIdentity();

    // Полная самопроверка. В обычном режиме пишется только при отказе, а с
    // !selftest=1 — всегда, чтобы можно было убедиться в разрешениях, ничего не
    // ломая. Пробы маунта откатываются сразу после себя.
    auto diagnose = [&dst, user_id]() {
        logPathLadder();
        const std::string opts =
            std::string(kSdcardFsOptions) + std::to_string(user_id);
        runMountProbes(dst, opts);
        dumpSelinuxDiagnostics(dst);
    };

    const bool selftest = g_state.cfg.selftest;

    // --- Уровень 1: как есть.
    if (tryAttach(dst, user_id, detail)) {
        if (selftest) diagnose();
        return true;
    }

    // --- Уровень 2: подменяем личность на владельцев нижнего слоя.
    // /data/media — 0550 uid 1023 (media_rw), /mnt/user — 0750 gid 1023,
    // /mnt/user/0 — 0710 gid 9997. setfsuid/setgroups требуют CAP_SETUID и
    // CAP_SETGID; если их нет, вызовы молча ничего не сделают, и это будет
    // видно по следующей строке лога.
    const uid_t prev_fsuid = setfsuid(kAidMediaRw);
    const gid_t prev_fsgid = setfsgid(kAidEverybody);

    gid_t saved_groups[64];
    const int saved_ngroups = getgroups(64, saved_groups);
    const gid_t want_groups[2] = {kAidMediaRw, kAidEverybody};
    errno = 0;
    const int groups_rc = setgroups(2, want_groups);

    LOGV("личность: setfsuid(%u) было %d, setfsgid(%u) было %d, setgroups -> %s",
         static_cast<unsigned>(kAidMediaRw), static_cast<int>(prev_fsuid),
         static_cast<unsigned>(kAidEverybody), static_cast<int>(prev_fsgid),
         groups_rc == 0 ? "ok" : strerror(errno));

    const bool attached = tryAttach(dst, user_id, detail);

    // Возвращаем личность: Zygote дальше сам выставит uid/gid/groups ребёнка,
    // но полагаться на это не будем.
    if (saved_ngroups >= 0) {
        setgroups(static_cast<size_t>(saved_ngroups), saved_groups);
    }
    setfsuid(prev_fsuid);
    setfsgid(prev_fsgid);

    if (attached) {
        if (selftest) diagnose();
        return true;
    }

    // --- Уровень 3: полная диагностика. Сюда попадаем только если не помогло.
    diagnose();

    detail = "bind и прямой маунт не удались (" + dst + ")";
    return false;
}

// ---------------------------------------------------------------- модуль

class SdcardFsRestore : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        // Обрабатываем ДВА режима:
        //   DEFAULT          — все обычные приложения;
        //   ANDROID_WRITABLE — приложения с MANAGE_EXTERNAL_STORAGE и системный
        //                      провайдер SAF com.android.externalstorage, через
        //                      который ходит DocumentsUI. Без него «Файлы»
        //                      показывают Android/data и Android/obb пустыми:
        //                      провайдер остаётся на FUSE, а поверх неё лежит
        //                      mirror-маунт сырой ФС (/data/media/0/Android/data,
        //                      режим 2771 uid/gid 1023), листинг которого ему
        //                      запрещён по DAC — остаётся только --x.
        // Пропускаем только настоящих потребителей сырого /data/media:
        //   NONE         — изолированные процессы (внешнего хранилища нет вовсе)
        //   INSTALLER    — installd
        //   PASS_THROUGH — MediaProvider (ему нужен сырой /data/media)
        // Им подмена сломала бы работу хранилища.
        const int mode = args->mount_external;
        const bool is_default = (mode == kMountModeExternalDefault);
        const bool is_android_writable = (mode == kMountModeExternalAndroidWritable);
        if (!is_default && !is_android_writable) {
            LOGV("пропуск (%s): %s", mountModeName(mode), resolvePackage(args).c_str());
            return;
        }

        ensureConfig();
        if (!g_state.cfg.enabled) return;

        const unsigned user_id = static_cast<unsigned>(args->uid) / kAidUserOffset;

        // Точка (или точки), которую Zygote заберёт под /storage. Для
        // ANDROID_WRITABLE их может быть две — выбор зависит от
        // persist.sys.vold_app_data_isolation_enabled. Накрываем все, которые
        // есть в namespace, чтобы не зависеть от значения свойства.
        std::vector<std::string> targets;
        targets.push_back("/mnt/user/" + std::to_string(user_id) + "/emulated");
        if (is_android_writable) {
            targets.push_back("/mnt/androidwritable/" + std::to_string(user_id) +
                              "/emulated");
        }

        std::string detail;
        bool ok = false;
        for (const std::string &t : targets) {
            std::string one;
            if (attachSdcardFs(user_id, t, one)) {
                ok = true;
                if (!detail.empty()) detail += "; ";
                detail += t + " (" + one + ")";
            } else {
                LOGV("не удалось накрыть %s: %s", t.c_str(), one.c_str());
            }
        }

        if (!ok) {
            LOGW("не удалось подключить sdcardfs (uid=%d user=%u, %s): %s", args->uid,
                 user_id, resolvePackage(args).c_str(), detail.c_str());
            return;
        }

        LOGI("sdcardfs подключён (uid=%d user=%u, %s): %s", args->uid, user_id,
             resolvePackage(args).c_str(), detail.c_str());

        // Android/data и Android/obb отдаём напрямую, как до scoped storage.
        //
        // Штатно Zygote (SpecializeCommon → BindMountStorageDirs) накрывает
        // /storage/emulated/<u>/Android/{data,obb} временным tmpfs и биндит туда
        // ТОЛЬКО каталоги своего пакета — остальные пакеты не видны вообще.
        // Флаг mount_storage_dirs (jboolean* из аргументов nativeForkAndSpecialize)
        // это отключает. Zygisk передаёт в модуль указатель на локальную
        // переменную хука, и та же переменная уходит в оригинальный
        // nativeForkAndSpecialize, поэтому изменение доезжает до Zygote
        // (см. Magisk native/src/core/zygisk/jni_hooks.hpp).
        if (g_state.cfg.raw_android_dirs) {
            if (args->mount_storage_dirs == nullptr) {
                LOGW("mount_storage_dirs недоступен (старый Zygisk?) — "
                     "Android/{data,obb} останутся изолированными");
            } else if (*args->mount_storage_dirs != JNI_FALSE) {
                *args->mount_storage_dirs = JNI_FALSE;
                LOGV("mount_storage_dirs -> false: Android/{data,obb} отдаются целиком");
            }
        }
    }

private:
    // Нужен только для читаемых логов: подменой он не управляет.
    std::string resolvePackage(zygisk::AppSpecializeArgs *args) {
        if (env_ == nullptr) return std::string();

        if (args->app_data_dir != nullptr) {
            const char *dir = env_->GetStringUTFChars(args->app_data_dir, nullptr);
            if (dir != nullptr) {
                const char *slash = strrchr(dir, '/');
                std::string pkg =
                    (slash != nullptr && slash[1] != '\0') ? (slash + 1) : "";
                env_->ReleaseStringUTFChars(args->app_data_dir, dir);
                if (!pkg.empty()) return pkg;
            }
        }

        if (args->nice_name != nullptr) {
            const char *name = env_->GetStringUTFChars(args->nice_name, nullptr);
            if (name != nullptr) {
                std::string pkg = name;
                env_->ReleaseStringUTFChars(args->nice_name, name);
                size_t colon = pkg.find(':');
                if (colon != std::string::npos) pkg.resize(colon);
                return pkg;
            }
        }

        return std::string();
    }

    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
};

}  // namespace

REGISTER_ZYGISK_MODULE(SdcardFsRestore)
