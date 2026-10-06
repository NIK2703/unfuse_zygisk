/*
 * hook_libc.cpp — userspace-эмуляция того, что sdcardfs делает с режимами.
 *
 * Зачем. На сыром /data/media доступ держится на POSIX ACL с именованной
 * записью для группы 9997 (AID_EVERYBODY), которую расставляет storage-fix.
 * Но ACL — не виртуализация: ядро применяет его ровно к тому режиму, который
 * запросило приложение. Если приложение создаёт файл с режимом 0600 (или каталог
 * 0700, или делает chmod 0600), posix_acl_create_masq() обнуляет маску ACL —
 * именованная запись 9997 перестаёт действовать, и чужие приложения получают
 * EACCES. sdcardfs такого не допускает: он вообще не показывает нижний режим,
 * а синтезирует каталогам 0770, файлам 0660 с gid 9997 (mount mask=0007).
 *
 * Что делает этот файл. Правит входные точки bionic в процессе приложения,
 * чтобы запрошенный режим приводился к «sdcardfs-виду» ещё до сисколла:
 *
 *   open/openat/creat/...   режим получает rw для группы, other обнуляется
 *   mkdir/mkdirat           то же плюс x для группы
 *   chmod/fchmod/fchmodat   то же — sdcardfs делает chmod полным no-op, здесь
 *                           режим хотя бы не сужается
 *   rename/link             объект, принесённый из приватного каталога, получает
 *                           ACL: при rename default ACL не применяется вовсе, и
 *                           файл приходит вообще без него
 *   mkstemp и родня         0600 меняется на 0660
 *
 * Патчатся не все перечисленные имена, а только «корни» — функции, которые сами
 * выполняют работу: open, openat, __open_2, __openat_2, fchmod, fchmodat,
 * mkdirat, renameat2, linkat. Остальные (creat, mkdir, chmod, link, rename,
 * renameat, mkstemp, mkostemp, mkstemps, mkostemps) в libc — переходники в
 * 8–16 байт, и записывать в них 16 байт нельзя: патч затрёт начало следующей
 * функции. Патчить их и не нужно — они переходят через .plt на корень.
 * Разбор с адресами и таблицей релокаций — в комментарии к tail_call_target().
 *
 * Почему правится вход функции, а не адрес в PLT. Вход правится один раз,
 * поэтому хук ловит и вызовы из уже загруженных библиотек, и из тех, что
 * приложение подгрузит позже, и вызовы через указатель, полученный из dlsym.
 * Трамплин не нужен: обработчик не зовёт оригинал, а делает сисколл сам —
 * поэтому нет ни копирования инструкций, ни их релокации.
 *
 * Обработчики обязаны быть реентерабельными: только сисколлы, никаких блокировок,
 * выделения памяти и stdio. Стек — фиксированные буферы.
 *
 * Только arm64. Правка входа — это запись 16 байт машинного кода, у каждой
 * архитектуры своей; на остальных ABI модуль собирается, но хуки не ставятся.
 *
 * Перед выпуском набор целей проверяется на настоящей libc с устройства:
 *   tools/verify-hook-targets.py device/libc/libc-arm64.so
 * Инструмент сверяет размеры функций с шириной патча и доказывает покрытие
 * пропущенных имён через .plt по таблице релокаций.
 */

#include "hook_libc.h"

#include "func_size.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <dlfcn.h>

#include <android/log.h>

#define LOG_TAG "SdcardFsRestore"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// AID_EVERYBODY из android_filesystem_config.h — общая группа всех приложений
// одного профиля; ровно её используют маунты sdcardfs read/write/full.
constexpr uint32_t kAidEverybody = 9997;

// Переходник не бывает длиннее: это перестановки аргументов плюс один переход.
// Нужен только на arm64 (см. tail_call_target), отсюда пометка.
[[maybe_unused]] constexpr unsigned kThunkMax = 32;

// xattr-имена POSIX ACL
constexpr const char *kAclAccess = "system.posix_acl_access";
constexpr const char *kAclDefault = "system.posix_acl_default";

// linux/include/uapi/linux/posix_acl_xattr.h
constexpr uint16_t kAclUserObj = 0x01;
constexpr uint16_t kAclGroupObj = 0x04;
constexpr uint16_t kAclGroup = 0x08;
constexpr uint16_t kAclMask = 0x10;
constexpr uint16_t kAclOther = 0x20;
constexpr uint32_t kAclVersion = 0x0002;

struct acl_entry {
    uint16_t e_tag;
    uint16_t e_perm;
    uint32_t e_id;
};

// ------------------------------------------------------------------ пути

// Префиксы, под которыми живёт общее хранилище. /storage/ покрывает и
// /storage/emulated, и /storage/self, и карту памяти; /mnt/user и /mnt/runtime —
// то же дерево, но до бинда Zygote; /mnt/pass_through — точка AOSP для FUSE;
// /data/media — сырой том, которым пользуются привилегированные потребители.
constexpr const char *kStoragePrefixes[] = {
    "/storage/",
    "/mnt/user/",
    "/mnt/runtime/",
    "/mnt/pass_through/",
    "/data/media/",
};

// /sdcard — симлинк на /storage/self/primary, и он должен совпасть целиком,
// иначе "/sdcardfoo" ложно попадёт в хранилище.
bool sdcard_prefix(const char *path) {
    static constexpr char k[] = "/sdcard";
    if (strncmp(path, k, sizeof(k) - 1) != 0) return false;
    const char c = path[sizeof(k) - 1];
    return c == '\0' || c == '/';
}

bool absolute_is_storage(const char *path) {
    for (const char *p : kStoragePrefixes) {
        if (strncmp(path, p, strlen(p)) == 0) return true;
    }
    return sdcard_prefix(path);
}

// "/proc/self/fd/<n>" без snprintf: обработчик должен обходиться без stdio.
void fd_link_path(int fd, char *out, size_t len) {
    static constexpr char kPfx[] = "/proc/self/fd/";
    size_t i = 0;
    while (i < sizeof(kPfx) - 1 && i + 1 < len) out[i] = kPfx[i], i++;

    char num[12];
    int n = 0;
    if (fd == 0) {
        num[n++] = '0';
    }
    for (int v = fd; v > 0 && n < (int)sizeof(num); v /= 10) num[n++] = (char)('0' + v % 10);
    while (n > 0 && i + 1 < len) out[i++] = num[--n];
    out[i] = '\0';
}

// Открыт ли дескриптор на общем хранилище. Нужно для fchmod/fchmodat.
bool fd_is_storage(int fd) {
    char link[32];
    fd_link_path(fd, link, sizeof link);
    char target[512];
    const long n = syscall(SYS_readlinkat, AT_FDCWD, link, target, sizeof target - 1);
    if (n <= 0) return false;
    target[n] = '\0';
    return absolute_is_storage(target);
}

bool is_storage(int dirfd, const char *path) {
    if (path == nullptr) return false;
    if (path[0] == '/') return absolute_is_storage(path);
    if (dirfd != AT_FDCWD) return fd_is_storage(dirfd);

    // Относительный путь от текущего каталога. Приложения почти всегда работают
    // абсолютными путями, так что сюда попадаем редко; но нативный код с chdir()
    // существует, и ошибиться в сторону «не хранилище» здесь нельзя.
    char cwd[512];
    const long n = syscall(SYS_getcwd, cwd, sizeof cwd);
    if (n <= 0) return false;
    return absolute_is_storage(cwd);
}

// ------------------------------------------------------- приведение режима

// Так выглядел бы запрошенный режим на sdcardfs: права владельца сохраняются,
// группе гарантированы rw (и x для каталогов), other обнулён — sdcardfs делает
// это своей маской 0007. Обнуление other важно не для доступа приложений (они
// и так в группе 9997), а для точности: на sdcardfs файл 0666 виден как 0660.
mode_t as_sdcardfs_file(mode_t m) {
    return (m & S_IRWXU) | (m & S_IRWXG) | S_IRGRP | S_IWGRP;
}

mode_t as_sdcardfs_dir(mode_t m) {
    return (m & S_IRWXU) | (m & S_IRWXG) | S_IRGRP | S_IWGRP | S_IXGRP;
}

// Режим приводится по типу существующего объекта: у chmod каталог и файл
// требуют разных битов группы.
mode_t widen_existing(int dirfd, const char *path, mode_t mode, int at_flags) {
    struct stat st;
    if (fstatat(dirfd, path, &st, at_flags) == 0 && S_ISDIR(st.st_mode)) {
        return as_sdcardfs_dir(mode);
    }
    return as_sdcardfs_file(mode);
}

// ---------------------------------------------------------------- ACL

// Собирает пять записей ACL так же, как vold::SetDefaultAcl (vold-16/Utils.cpp:142)
// и как tools/storage-fix.c. mode задаёт права владельца, группы и остальных;
// именованная запись для 9997 получает права группы, и они же идут в маску —
// иначе маска обнулила бы выданный доступ.
//
// OTHER обнуляется всегда: sdcardfs делает это своей маской 0007, и на sdcardfs
// файл 0666 виден как 0660.
void acl_build(uint8_t *buf, size_t *len, mode_t mode) {
    const uint16_t g = static_cast<uint16_t>((mode & S_IRWXG) >> 3);

    acl_entry e[5];
    e[0] = {kAclUserObj, static_cast<uint16_t>((mode & S_IRWXU) >> 6), static_cast<uint32_t>(-1)};
    e[1] = {kAclGroupObj, g, static_cast<uint32_t>(-1)};
    e[2] = {kAclGroup, g, kAidEverybody};
    e[3] = {kAclMask, g, 0};
    e[4] = {kAclOther, 0, 0};

    memcpy(buf, &kAclVersion, sizeof(uint32_t));
    memcpy(buf + sizeof(uint32_t), e, sizeof(e));
    *len = sizeof(uint32_t) + sizeof(e);
}

int acl_write(const char *path, const char *name, mode_t mode) {
    uint8_t buf[sizeof(uint32_t) + 5 * sizeof(acl_entry)];
    size_t len = 0;
    acl_build(buf, &len, mode);
    return static_cast<int>(syscall(SYS_setxattr, path, name, buf, len, 0));
}

int acl_write_fd(int fd, const char *name, mode_t mode) {
    uint8_t buf[sizeof(uint32_t) + 5 * sizeof(acl_entry)];
    size_t len = 0;
    acl_build(buf, &len, mode);
    return static_cast<int>(syscall(SYS_fsetxattr, fd, name, buf, len, 0));
}

// Приводит в порядок объект по открытому дескриптору: сначала режим (он же
// задаёт маску ACL), затем ACL. Порядок важен: chmod переписывает
// USER_OBJ/GROUP_OBJ/MASK/OTHER, поэтому ACL должен лечь последним.
//
// Вариант через дескриптор предпочтителен там, где дескриптор уже есть: он не
// зависит ни от текущего каталога, ни от гонки с подменой пути.
void fix_fd(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return;
    if (S_ISLNK(st.st_mode)) return;
    const bool dir = S_ISDIR(st.st_mode);
    if (!dir && !S_ISREG(st.st_mode)) return;

    const mode_t want = dir ? as_sdcardfs_dir(st.st_mode) : as_sdcardfs_file(st.st_mode);
    syscall(SYS_fchmod, fd, want);
    acl_write_fd(fd, kAclAccess, want);
    if (dir) acl_write_fd(fd, kAclDefault, want);
}

// То же по пути — для rename и link, где дескриптора нет.
void fix_object(const char *path) {
    struct stat st;
    if (fstatat(AT_FDCWD, path, &st, AT_SYMLINK_NOFOLLOW) != 0) return;
    if (S_ISLNK(st.st_mode)) return;
    const bool dir = S_ISDIR(st.st_mode);
    if (!dir && !S_ISREG(st.st_mode)) return;

    const mode_t want = dir ? as_sdcardfs_dir(st.st_mode) : as_sdcardfs_file(st.st_mode);

    syscall(SYS_fchmodat, AT_FDCWD, path, want, 0);
    acl_write(path, kAclAccess, want);
    if (dir) acl_write(path, kAclDefault, want);
}

// Каталог только что создан нами, но дескриптора у нас нет: открываем его и
// правим через дескриптор — так не нужен ни абсолютный путь, ни /proc, и
// относительный путь от dirfd обрабатывается сам собой.
void fix_created_dir(int dirfd, const char *path) {
    const int fd = static_cast<int>(
        syscall(SYS_openat, dirfd, path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0));
    if (fd < 0) return;
    fix_fd(fd);
    syscall(SYS_close, fd);
}

// Существует ли объект — нужно, чтобы отличить создание от открытия уже
// имеющегося файла: ACL правится только первому.
bool exists_at(int dirfd, const char *path) {
    struct stat st;
    return fstatat(dirfd, path, &st, 0) == 0;
}

// ------------------------------------------------------------- обработчики
//
// Все обработчики заменяют оригинал целиком и делают сисколл сами. Поэтому они
// не зависят ни от порядка правки, ни от того, попал ли вызов через PLT.

bool needs_mode(int flags) {
    return (flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE;
}

// Общий путь для всех «открыть с созданием»: привести режим, сделать сисколл и,
// если объект действительно появился, дописать ему ACL.
//
// Почему одного режима мало. Доступ чужим приложениям даёт не режим, а
// именованная запись ACL для группы 9997. Наследуется она из default ACL
// каталога — а default ACL вещь хрупкая: vold пересобирает /data/media/<user>,
// Android, Android/data, Android/obb и Android/media при каждой загрузке, и
// порядок «скрипт модуля / vold» не гарантирован. Проверено на устройстве:
// у /data/media/0 access ACL был с 9997, а default — с 1023 (media_rw), то есть
// созданный в корне хранилища файл наследовал 1023 и приложениям был не виден.
//
// Дописывание ACL здесь делает гарантию локальной: что бы ни лежало в дереве,
// созданное приложением сразу видно всем приложениям.
int open_and_fix(int dirfd, const char *path, int flags, mode_t mode) {
    const bool storage = needs_mode(flags) && is_storage(dirfd, path);
    const bool existed = storage && exists_at(dirfd, path);
    if (storage) mode = as_sdcardfs_file(mode);

    const int fd = static_cast<int>(syscall(SYS_openat, dirfd, path, flags, mode));
    if (fd >= 0 && storage && !existed) fix_fd(fd);
    return fd;
}

extern "C" int h_open(const char *path, int flags, mode_t mode) {
    return open_and_fix(AT_FDCWD, path, flags, mode);
}

extern "C" int h_openat(int dirfd, const char *path, int flags, mode_t mode) {
    return open_and_fix(dirfd, path, flags, mode);
}

extern "C" int h_creat(const char *path, mode_t mode) {
    return open_and_fix(AT_FDCWD, path, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

// __open_2/__openat_2 — варианты без режима, которые fortify подставляет, когда
// режим заведомо не нужен. Если O_CREAT всё-таки пришёл (это ошибка вызывающего,
// bionic в таком случае падает), берём безопасный 0660, а не нулевой режим:
// нулевой обнулил бы маску ACL и сломал бы доступ.
extern "C" int h_open_2(const char *path, int flags) {
    return open_and_fix(AT_FDCWD, path, flags, needs_mode(flags) ? 0666 : 0);
}

extern "C" int h_openat_2(int dirfd, const char *path, int flags) {
    return open_and_fix(dirfd, path, flags, needs_mode(flags) ? 0666 : 0);
}

extern "C" int h_mkdirat(int dirfd, const char *path, mode_t mode);

extern "C" int h_mkdir(const char *path, mode_t mode) {
    return h_mkdirat(AT_FDCWD, path, mode);
}

extern "C" int h_mkdirat(int dirfd, const char *path, mode_t mode) {
    const bool storage = is_storage(dirfd, path);
    if (storage) mode = as_sdcardfs_dir(mode);

    const int r = static_cast<int>(syscall(SYS_mkdirat, dirfd, path, mode));
    if (r == 0 && storage) fix_created_dir(dirfd, path);
    return r;
}

extern "C" int h_chmod(const char *path, mode_t mode) {
    if (is_storage(AT_FDCWD, path)) mode = widen_existing(AT_FDCWD, path, mode, 0);
    return static_cast<int>(syscall(SYS_fchmodat, AT_FDCWD, path, mode, 0));
}

extern "C" int h_fchmodat(int dirfd, const char *path, mode_t mode, int at_flags) {
    if (is_storage(dirfd, path)) mode = widen_existing(dirfd, path, mode, at_flags);
    return static_cast<int>(syscall(SYS_fchmodat, dirfd, path, mode, at_flags));
}

extern "C" int h_fchmod(int fd, mode_t mode) {
    if (fd_is_storage(fd)) {
        struct stat st;
        if (syscall(SYS_fstat, fd, &st) == 0) {
            mode = S_ISDIR(st.st_mode) ? as_sdcardfs_dir(mode) : as_sdcardfs_file(mode);
        }
    }
    return static_cast<int>(syscall(SYS_fchmod, fd, mode));
}

// rename/link правятся после операции и только когда объект пришёл в хранилище
// извне: default ACL при rename не применяется, поэтому файл 0600 из
// /data/data/<pkg> оказался бы в /sdcard без ACL вообще.
void fix_after_move(int dirfd, const char *dst, int src_dirfd, const char *src) {
    if (dst == nullptr) return;
    if (!is_storage(dirfd, dst)) return;
    if (src != nullptr && is_storage(src_dirfd, src)) return;
    fix_object(dst);
}

extern "C" int h_rename(const char *oldp, const char *newp) {
    const int r = static_cast<int>(syscall(SYS_renameat, AT_FDCWD, oldp, AT_FDCWD, newp));
    if (r == 0) fix_after_move(AT_FDCWD, newp, AT_FDCWD, oldp);
    return r;
}

extern "C" int h_renameat(int olddirfd, const char *oldp, int newdirfd, const char *newp) {
    const int r = static_cast<int>(syscall(SYS_renameat, olddirfd, oldp, newdirfd, newp));
    if (r == 0) fix_after_move(newdirfd, newp, olddirfd, oldp);
    return r;
}

extern "C" int h_renameat2(int olddirfd, const char *oldp, int newdirfd, const char *newp,
                           unsigned flags) {
    const int r = static_cast<int>(syscall(SYS_renameat2, olddirfd, oldp, newdirfd, newp, flags));
    if (r == 0) fix_after_move(newdirfd, newp, olddirfd, oldp);
    return r;
}

extern "C" int h_link(const char *oldp, const char *newp) {
    const int r = static_cast<int>(syscall(SYS_linkat, AT_FDCWD, oldp, AT_FDCWD, newp, 0));
    if (r == 0) fix_after_move(AT_FDCWD, newp, AT_FDCWD, oldp);
    return r;
}

extern "C" int h_linkat(int olddirfd, const char *oldp, int newdirfd, const char *newp, int flags) {
    const int r = static_cast<int>(syscall(SYS_linkat, olddirfd, oldp, newdirfd, newp, flags));
    if (r == 0) fix_after_move(newdirfd, newp, olddirfd, oldp);
    return r;
}

// ------------------------------------------------------------- mkstemp
//
// mkstemp создаёт файл с 0600 — ровно тот случай, который обнуляет маску ACL.
// Вызвать оригинал нельзя (его вход затёрт, а трамплин здесь был бы лишним
// риском), поэтому алгоритм повторён: подстановка случайных символов в шесть
// «X» и open(O_CREAT|O_EXCL) с повтором на EEXIST. Контракт тот же.

constexpr char kLetters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

uint64_t rand64() {
    uint64_t v = 0;
    if (syscall(SYS_getrandom, &v, sizeof v, 0) == static_cast<long>(sizeof v)) return v;

    // getrandom недоступен — берём монотонное время и tid.
    struct {
        long sec;
        long nsec;
    } ts {};
    syscall(SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */, &ts);
    uint64_t f = static_cast<uint64_t>(ts.nsec) * 2654435761u;
    f ^= static_cast<uint64_t>(ts.sec) << 17;
    f ^= static_cast<uint64_t>(syscall(SYS_gettid)) * 40503u;
    return f;
}

int mkstemp_impl(char *tmpl, int suffixlen, int extra_flags, mode_t mode) {
    if (tmpl == nullptr) {
        errno = EINVAL;
        return -1;
    }
    const size_t len = strlen(tmpl);
    if (suffixlen < 0 || len < static_cast<size_t>(6 + suffixlen)) {
        errno = EINVAL;
        return -1;
    }

    char *x = tmpl + len - 6 - static_cast<size_t>(suffixlen);
    for (int i = 0; i < 6; i++) {
        if (x[i] != 'X') {
            errno = EINVAL;
            return -1;
        }
    }

    for (int attempt = 0; attempt < 128; attempt++) {
        const uint64_t r = rand64();
        for (int i = 0; i < 6; i++) x[i] = kLetters[(r >> (i * 6)) & 63];

        const int fd = static_cast<int>(
            syscall(SYS_openat, AT_FDCWD, tmpl, O_CREAT | O_EXCL | O_RDWR | extra_flags, mode));
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }

    errno = EEXIST;
    return -1;
}

// 0600 сохраняется вне хранилища: там файл приватный по делу, и режим трогать
// нельзя. В хранилище он всё равно был бы виден как 0660.
mode_t mkstemp_mode(const char *tmpl) {
    return is_storage(AT_FDCWD, tmpl) ? as_sdcardfs_file(0600) : 0600;
}

extern "C" int h_mkstemp(char *tmpl) { return mkstemp_impl(tmpl, 0, 0, mkstemp_mode(tmpl)); }

extern "C" int h_mkostemp(char *tmpl, int flags) {
    return mkstemp_impl(tmpl, 0, flags, mkstemp_mode(tmpl));
}

extern "C" int h_mkstemps(char *tmpl, int suffixlen) {
    return mkstemp_impl(tmpl, suffixlen, 0, mkstemp_mode(tmpl));
}

extern "C" int h_mkostemps(char *tmpl, int suffixlen, int flags) {
    return mkstemp_impl(tmpl, suffixlen, flags, mkstemp_mode(tmpl));
}

// ------------------------------------------------------------- установка

#if defined(__aarch64__)

// 16 байт: ldr x17, #8; br x17; .quad <обработчик>.
//
// Выбрана именно эта последовательность, потому что литерал читается
// PC-относительно, а сам переход — по регистру: ни дальности, ни релокации
// инструкций оригинала не требуется. Запись литерала идёт первой, чтобы в момент
// появления перехода адрес обработчика уже лежал на месте.
constexpr uint32_t kLdrX17 = 0x58000051u;  // ldr x17, #8
constexpr uint32_t kBrX17 = 0xd61f0220u;   // br  x17
constexpr size_t kPatchSize = 16;

bool patch_entry(void *target, void *handler) {
    const uintptr_t addr = reinterpret_cast<uintptr_t>(target);
    if ((addr & 3u) != 0) {
        LOGE("вход %p не выровнен по 4 байта", target);
        return false;
    }

    const long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) return false;
    const uintptr_t page = addr & ~(static_cast<uintptr_t>(ps) - 1);

    // Патч может пересечь границу страницы — тогда нужны обе.
    const size_t span = static_cast<size_t>(addr - page) + kPatchSize;
    const size_t mlen = (span + static_cast<size_t>(ps) - 1) & ~(static_cast<size_t>(ps) - 1);

    // PROT_EXEC не снимается: отображение .text уже имеет VM_EXEC, и добавление
    // записи не считается «созданием исполняемой памяти» (иначе SELinux потребовал
    // бы process execmem). Запись ложится в приватную копию страницы — на диске
    // библиотека не меняется.
    if (mprotect(reinterpret_cast<void *>(page), mlen,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("mprotect(%p, %zu) -> %s", reinterpret_cast<void *>(page), mlen, strerror(errno));
        return false;
    }

    memcpy(reinterpret_cast<void *>(addr + 8), &handler, sizeof handler);
    reinterpret_cast<uint32_t *>(addr)[1] = kBrX17;
    reinterpret_cast<uint32_t *>(addr)[0] = kLdrX17;
    __builtin___clear_cache(reinterpret_cast<char *>(addr),
                            reinterpret_cast<char *>(addr) + kPatchSize);

    mprotect(reinterpret_cast<void *>(page), mlen, PROT_READ | PROT_EXEC);
    return true;
}

// Если вход функции — «переходник» (короткое тело из перестановок аргументов и
// одного хвостового перехода на настоящую реализацию), возвращает адрес цели
// перехода, иначе nullptr.
//
// Признак переходника — сочетание двух свойств: функция КОРОТКА и заканчивается
// безусловным переходом. По отдельности ни одно не годится:
//
//   * один хвостовой b ничего не значит: большая функция вправе закончиться
//     хвостовым вызовом (open заканчивается переходом на внутренний __openat);
//   * одна короткость ничего не значит: короткая функция может быть настоящей
//     реализацией, и тогда её как раз надо патчить.
//
// У переходников bionic перестановки аргументов стоят в НАЧАЛЕ, а переход — в
// конце, поэтому проверять первую инструкцию бесполезно. Вот creat (12 байт):
//
//     84f68: mov w2, w1        ; аргументы
//     84f6c: mov w1, #0x241
//     84f70: b   open@plt      ; хвостовой переход
//
// Ровно такие функции и опасны: 12 байт, а патч — 16, и он затрёт начало open.
// Патчить их и не нужно: переход идёт через .plt, а bionic собирает libc без
// -Bsymbolic, поэтому внутрибиблиотечные вызовы идут через GOT, и GOT указывает
// на ту самую точку входа, которую мы уже пропатчили. Проверено по таблице
// релокаций libc-16 (R_AARCH64_JUMP_SLOT):
//
//   creat/creat64 -> open@plt       -> open      (0x84f74)
//   renameat      -> renameat2@plt  -> renameat2 (0xe2180)
//   mkstemps/mkostemps -> mktemp_internal -> open@plt
//
// Эта функция нужна дважды: по ней движок пропускает переходники (см.
// hooks_install) и по ней же в отчёте видно, какие имена пропущены осознанно.
//
// Решающее условие БЕЗОПАСНОСТИ — всё равно размер: патч шириной 16 байт можно
// писать только в функцию не короче 16 байт. Переходник длиннее патча (mkdir —
// ровно 16 байт) патчить МОЖНО, но не нужно: вызовы к нему и так придут на
// пропатченный корень через .plt, а лишняя правка — лишний риск. Тем же
// условием пользуется предварительная проверка tools/verify-hook-targets.py,
// поэтому оба инструмента называют одни и те же девять корней.
void *tail_call_target(const void *fn, unsigned size) {
    if (size < 4 || size > kThunkMax) return nullptr;

    const uint32_t insn = static_cast<const uint32_t *>(fn)[size / 4 - 1];

    // Ищем именно B (0010 0110 imm26), а не BL: вызов с возвратом в конце
    // функции — это обычное тело, а не хвостовой переход.
    if ((insn & 0xfc000000u) != 0x14000000u) return nullptr;

    // imm26 — смещение в инструкциях (то есть в 4 байтах), со знаком.
    int32_t off = static_cast<int32_t>(insn & 0x03ffffffu);
    if ((off & 0x02000000) != 0) off -= 0x04000000;

    return const_cast<uint32_t *>(static_cast<const uint32_t *>(fn)) + off;
}

#else

constexpr size_t kPatchSize = 16;

bool patch_entry(void *, void *) {
    LOGE("правка входов libc реализована только для arm64");
    return false;
}

void *tail_call_target(const void *, unsigned) { return nullptr; }

#endif

enum class State { Failed, Ok, Alias, Missing, Thunk, Small, Unknown };

struct HookDef {
    const char *name;
    void *handler;
};

// Патчатся только корни — функции, которые сами выполняют работу. Переходники
// (creat, mkdir, chmod, link, rename, renameat, mkstemp и родня) в таблице
// остаются, но будут распознаны как переходники и пропущены: их вызовы идут
// через .plt на уже пропатченный корень. Держать их в таблице, а не вычеркнуть,
// стоит по двум причинам: на другой прошивке переходник может оказаться
// настоящей реализацией, и тогда его полезно пропатчить; и в отчёте видно, что
// про каждое имя решение принято осознанно.
const HookDef kHooks[] = {
    {"open", reinterpret_cast<void *>(h_open)},
    {"open64", reinterpret_cast<void *>(h_open)},
    {"openat", reinterpret_cast<void *>(h_openat)},
    {"openat64", reinterpret_cast<void *>(h_openat)},
    {"creat", reinterpret_cast<void *>(h_creat)},
    {"creat64", reinterpret_cast<void *>(h_creat)},
    {"__open_2", reinterpret_cast<void *>(h_open_2)},
    {"__openat_2", reinterpret_cast<void *>(h_openat_2)},
    {"mkdir", reinterpret_cast<void *>(h_mkdir)},
    {"mkdirat", reinterpret_cast<void *>(h_mkdirat)},
    {"chmod", reinterpret_cast<void *>(h_chmod)},
    {"fchmod", reinterpret_cast<void *>(h_fchmod)},
    {"fchmodat", reinterpret_cast<void *>(h_fchmodat)},
    {"rename", reinterpret_cast<void *>(h_rename)},
    {"renameat", reinterpret_cast<void *>(h_renameat)},
    {"renameat2", reinterpret_cast<void *>(h_renameat2)},
    {"link", reinterpret_cast<void *>(h_link)},
    {"linkat", reinterpret_cast<void *>(h_linkat)},
    {"mkstemp", reinterpret_cast<void *>(h_mkstemp)},
    {"mkostemp", reinterpret_cast<void *>(h_mkostemp)},
    {"mkstemps", reinterpret_cast<void *>(h_mkstemps)},
    {"mkostemps", reinterpret_cast<void *>(h_mkostemps)},
};

constexpr int kHookCount = static_cast<int>(sizeof(kHooks) / sizeof(kHooks[0]));

State g_state[kHookCount];
bool g_installed = false;

// open64 на 64-битных платформах — тот же адрес, что open: второй раз патчить
// нечего, помечаем как синоним.
void *g_patched[kHookCount];
int g_patched_n = 0;

bool already_patched(void *fn) {
    for (int i = 0; i < g_patched_n; i++) {
        if (g_patched[i] == fn) return true;
    }
    return false;
}

}  // namespace

int hooks_install(int *total) {
    if (total != nullptr) *total = kHookCount;
    if (g_installed) {
        int n = 0;
        for (int i = 0; i < kHookCount; i++) {
            if (g_state[i] == State::Ok || g_state[i] == State::Alias) n++;
        }
        return n;
    }
    g_installed = true;

    // Шаг 1: адреса. dlsym(RTLD_DEFAULT, ...) ищет в глобальной области, то есть
    // находит именно те определения, к которым придут вызовы из приложения.
    void *fns[kHookCount];
    for (int i = 0; i < kHookCount; i++) fns[i] = dlsym(RTLD_DEFAULT, kHooks[i].name);

    // Шаг 2: размеры. Без них патчить нельзя: 16 байт, записанные в 8- или
    // 12-байтовую функцию, затрут начало следующей. На libc-16 таких целей
    // пять, и все они — переходники, покрытые через .plt (см. tail_call_target).
    unsigned sizes[kHookCount];
    func_sizes(fns, kHookCount, sizes);

    // Шаг 3: решение. Патчим только то, что не короче патча и не является
    // переходником. Переходник тоже не короче патча может оказаться (mkdir —
    // ровно 16 байт), и его можно было бы пропатчить, но незачем: вызовы и так
    // придут на пропатченный корень, а лишняя правка — лишний риск.
    int ok = 0;
    int thunk = 0;
    int small = 0;
    int unknown = 0;
    for (int i = 0; i < kHookCount; i++) {
        void *fn = fns[i];
        if (fn == nullptr) {
            g_state[i] = State::Missing;
            continue;
        }
        if (already_patched(fn)) {
            g_state[i] = State::Alias;
            ok++;
            continue;
        }
        if (sizes[i] == 0) {
            g_state[i] = State::Unknown;
            unknown++;
            continue;
        }
        if (sizes[i] < kPatchSize) {
            g_state[i] = State::Small;
            small++;
            continue;
        }
        if (tail_call_target(fn, sizes[i]) != nullptr) {
            g_state[i] = State::Thunk;
            thunk++;
            continue;
        }
        if (patch_entry(fn, kHooks[i].handler)) {
            g_state[i] = State::Ok;
            if (g_patched_n < kHookCount) g_patched[g_patched_n++] = fn;
            ok++;
        } else {
            g_state[i] = State::Failed;
        }
    }

    LOGI("хуки libc: установлено %d из %d (переходников %d, короче патча %d, "
         "размер неизвестен %d)",
         ok, kHookCount, thunk, small, unknown);
    return ok;
}

void hooks_report(char *buf, size_t len) {
    if (buf == nullptr || len == 0) return;
    size_t used = 0;
    buf[0] = '\0';

    for (int i = 0; i < kHookCount; i++) {
        const char *s = "?";
        switch (g_state[i]) {
            case State::Ok: s = "ok"; break;
            case State::Alias: s = "alias"; break;
            case State::Missing: s = "нет"; break;
            case State::Failed: s = "СБОЙ"; break;
            case State::Thunk: s = "переходник"; break;
            case State::Small: s = "коротка"; break;
            case State::Unknown: s = "размер?"; break;
        }
        const int w = snprintf(buf + used, len - used, "%s%s=%s", used ? " " : "", kHooks[i].name, s);
        if (w < 0 || static_cast<size_t>(w) >= len - used) break;
        used += static_cast<size_t>(w);
    }
}

int hooks_path_is_storage(const char *path) {
    return (path != nullptr && path[0] == '/' && absolute_is_storage(path)) ? 1 : 0;
}
