/*
 * storage-fix — права на сыром дереве /data/media для ядер без sdcardfs.
 *
 * Зачем. sdcardfs сам переписывает права: он берёт маску и gid из опций маунта и
 * отдаёт любой процесс из группы 9997 (AID_EVERYBODY) каталоги 0770 и файлы 0660,
 * независимо от того, что лежит на нижней ФС. Без sdcardfs ничего этого нет, а
 * дерево /data/media принадлежит 1023:1023 (media_rw) с режимами 0550/2770/0670 —
 * то есть приложения (в группах которых нет 1023) не могут даже войти в каталог.
 *
 * Что делает. Расставляет POSIX ACL с именованной записью для группы 9997 —
 * ровно тот же gid, который используют sdcardfs-маунты read/write/full
 * (system-core-16/sdcard/sdcard.cpp:182-206). Группа 9997 по определению общая
 * для всех приложений (system-core-16/libcutils/include/private/
 * android_filesystem_config.h:166), поэтому доступ получают все приложения.
 *
 * Почему ACL, а не chmod. Режим файла — один на всю систему, а ACL-запись
 * действует поверх него и не мешает ни MediaProvider, ни vold. Кроме того:
 *
 *   - vold при каждой загрузке сбрасывает владельца и режим /data/media и
 *     Android/ (fs_prepare_dir → chown+chmod). chmod правит в ACL только
 *     USER_OBJ/GROUP_OBJ/MASK/OTHER и НЕ трогает именованные записи, поэтому
 *     выданный здесь доступ переживает перезагрузку и повторный chown/chmod.
 *   - umask приложений равен 0077, из-за чего новые файлы получались бы 0600.
 *     Но если у каталога есть default ACL, ядро не применяет umask вообще:
 *     vfs_create() пропускает `mode &= ~current_umask()`, а posix_acl_create()
 *     считает режим пересечением с ACL. Поэтому файл, созданный приложением,
 *     выходит 0660 с унаследованной записью для 9997 — и его сразу видят все.
 *
 * Каталогам ставится и access-, и default-ACL: первая открывает сам каталог,
 * вторая наследуется всем, что в нём создадут (в том числе vold и MediaProvider).
 *
 * ---------------------------------------------------------------------------
 * Режимы:
 *
 *   storage-fix <каталог>...          — выдать группе 9997 rwx/rw рекурсивно
 *   storage-fix --traverse <каталог>  — только r-x на сам каталог (для корня
 *                                       тома: его нужно пройти, но не писать в него)
 *   storage-fix --check <каталог>...  — проверить, что запись 9997 есть в ACL
 *                                       (код 0 — всё на месте, 1 — нет)
 *   storage-fix --guard               — сторож: держит инвариант по событиям
 *
 * ---------------------------------------------------------------------------
 * Зачем сторож.
 *
 * Одного прохода мало, и это не теория. vold пишет СВОИ default-ACL уже после
 * скриптов модуля, причём в разные моменты — подготовка CE-хранилища
 * пользователя идёт от фреймворка (в post-fs-data vold делает только DE,
 * vold-16/FsCrypt.cpp:657), а каталоги пакетов создаются вообще когда угодно,
 * вплоть до установки приложения через час после загрузки.
 *
 * Каждый такой вызов — setxattr(system.posix_acl_default), а он ЗАМЕНЯЕТ ACL
 * целиком, вместе с нашей записью для 9997 (vold::SetDefaultAcl собирает ACL с
 * нуля, vold-16/Utils.cpp:142). Мест ровно три:
 *
 *   /data/media/<user>                     FsCrypt.cpp:1027  запись для 1023
 *   /data/media/<user>/Android/obb         Utils.cpp:1889    записи нет вовсе
 *   /data/media/<user>/Android/{data,obb,media}/<pkg>
 *                                          Utils.cpp:406     запись для uid пакета
 *
 * Следствие: каталог, созданный в таком месте процессом, на который хуки libc
 * не распространяются (vold, MTP, root-демон), унаследует чужую запись вместо
 * 9997 — и приложения его не увидят.
 *
 * Поймать момент последней правки таймером нельзя: он зависит от разблокировки
 * и от того, когда система поставит очередной пакет. Править по факту события —
 * можно, и это ровно то, что даёт inotify: setxattr меняет метаданные inode, а
 * значит приходит как IN_ATTRIB на сторожимом каталоге (fsnotify_xattr →
 * FS_ATTRIB). Поэтому сторож:
 *
 *   * стережёт IN_ATTRIB на /data/media/<user> и на Android/{data,obb,media}
 *     вместе с каталогами пакетов — то есть ровно там, где vold зовёт
 *     SetDefaultAcl, плюс /data/media, чтобы поймать появление каталога
 *     пользователя;
 *   * на каждое событие СНАЧАЛА читает ACL и пишет, только если записи 9997
 *     нет, — иначе свои же правки завели бы бесконечный цикл событий;
 *   * раз в 10 секунд пересматривает свой список: на случай потерянного события
 *     и чтобы подхватить каталог, появившийся раньше сторожа.
 *
 * Ничего привязанного к сборке здесь нет и быть не может: сторожу безразлично,
 * кто и когда испортил ACL. Он держит инвариант, а не адрес инструкции в чужом
 * бинарнике, поэтому переживает и обновление прошивки, и смену версии vold.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define XATTR_ACL_ACCESS "system.posix_acl_access"
#define XATTR_ACL_DEFAULT "system.posix_acl_default"

/* linux/include/uapi/linux/posix_acl_xattr.h */
#define POSIX_ACL_XATTR_VERSION 0x0002
#define ACL_USER_OBJ 0x01
#define ACL_USER 0x02
#define ACL_GROUP_OBJ 0x04
#define ACL_GROUP 0x08
#define ACL_MASK 0x10
#define ACL_OTHER 0x20

/* android_filesystem_config.h: общая группа всех приложений одного профиля */
#define AID_EVERYBODY 9997

/* Сколько записей ACL готовы разобрать. Больше пяти не пишет ни vold, ни мы. */
#define ACL_MAX_ENTRIES 32

struct acl_entry {
    uint16_t e_tag;
    uint16_t e_perm;
    uint32_t e_id;
};

static unsigned long stat_dirs, stat_files, stat_skipped, stat_errors;

/* ==========================================================================
 * Разбор и проверка ACL
 * ========================================================================== */

/*
 * Читает ACL в массив записей.
 *   >= 0 — число записей;
 *   -1   — ошибка (errno сохранён);
 *   -2   — атрибута нет вовсе (для вызывающего это не ошибка, а «ACL не задана»).
 */
static int acl_read(const char *path, const char *name, struct acl_entry *out, size_t maxn) {
    uint8_t buf[sizeof(uint32_t) + ACL_MAX_ENTRIES * sizeof(struct acl_entry)];

    ssize_t len = getxattr(path, name, buf, sizeof(buf));
    if (len < 0) return errno == ENODATA ? -2 : -1;
    if (len < (ssize_t)sizeof(uint32_t)) {
        errno = EINVAL;
        return -1;
    }

    uint32_t version;
    memcpy(&version, buf, sizeof(version));
    if (version != POSIX_ACL_XATTR_VERSION) {
        errno = EINVAL;
        return -1;
    }

    size_t n = ((size_t)len - sizeof(uint32_t)) / sizeof(struct acl_entry);
    if (n > maxn) n = maxn;
    memcpy(out, buf + sizeof(uint32_t), n * sizeof(struct acl_entry));
    return (int)n;
}

/*
 * Есть ли в ACL именованная запись для 9997 и не срезана ли она маской.
 * Маска — не формальность: именно она решает, сколько от именованной записи
 * реально достанется процессу, и на диск она попадает из режима каталога.
 */
static int acl_allows_everybody(const char *path, const char *name, uint16_t *perm_out) {
    struct acl_entry e[ACL_MAX_ENTRIES];

    int n = acl_read(path, name, e, ACL_MAX_ENTRIES);
    if (n <= 0) return 0;

    uint16_t perm = 0, mask = 0;
    int have_mask = 0;

    for (int i = 0; i < n; i++) {
        if (e[i].e_tag == ACL_GROUP && e[i].e_id == AID_EVERYBODY) {
            perm = e[i].e_perm;
        } else if (e[i].e_tag == ACL_MASK) {
            mask = e[i].e_perm;
            have_mask = 1;
        }
    }

    if (perm == 0) return 0;
    if (have_mask && (perm & ~mask) != 0) return 0;
    if (perm_out) *perm_out = perm;
    return 1;
}

/* Инвариант: у каталога запись 9997 есть и в access-, и в default-ACL. */
static int acl_ok(const char *path, int is_dir) {
    if (!acl_allows_everybody(path, XATTR_ACL_ACCESS, NULL)) return 0;
    if (is_dir && !acl_allows_everybody(path, XATTR_ACL_DEFAULT, NULL)) return 0;
    return 1;
}

/* ==========================================================================
 * Правка ACL
 * ========================================================================== */

/*
 * Собирает ACL из пяти записей — так же, как vold::SetDefaultAcl
 * (vold-16/Utils.cpp:142). mode задаёт права владельца, группы и остальных;
 * именованная запись для 9997 получает права группы, и они же идут в маску,
 * иначе маска обнулила бы выданный доступ.
 */
static int acl_apply(const char *path, const char *name, mode_t mode) {
    struct acl_entry e[5];
    const uint16_t group_perm = (mode & S_IRWXG) >> 3;
    size_t n = 0;

    e[n].e_tag = ACL_USER_OBJ;
    e[n].e_perm = (mode & S_IRWXU) >> 6;
    e[n].e_id = (uint32_t)-1;
    n++;
    e[n].e_tag = ACL_GROUP_OBJ;
    e[n].e_perm = group_perm;
    e[n].e_id = (uint32_t)-1;
    n++;
    e[n].e_tag = ACL_GROUP;
    e[n].e_perm = group_perm;
    e[n].e_id = AID_EVERYBODY;
    n++;
    e[n].e_tag = ACL_MASK;
    e[n].e_perm = group_perm;
    e[n].e_id = 0;
    n++;
    e[n].e_tag = ACL_OTHER;
    e[n].e_perm = mode & S_IRWXO;
    e[n].e_id = 0;
    n++;

    uint8_t buf[sizeof(uint32_t) + 5 * sizeof(struct acl_entry)];
    const uint32_t version = POSIX_ACL_XATTR_VERSION;
    memcpy(buf, &version, sizeof(version));
    memcpy(buf + sizeof(version), e, n * sizeof(struct acl_entry));
    const size_t len = sizeof(version) + n * sizeof(struct acl_entry);

    return setxattr(path, name, buf, len, 0);
}

/*
 * Бит «остальных» (S_IRWXO) здесь СОЗНАТЕЛЬНО отбрасывается во всех трёх
 * функциях — и это не мелочь, а условие совместимости с хуком.
 *
 * Доступ к общему хранилищу выдаёт именованная запись ACL для группы 9997, а
 * не бит «остальных». Если оставить «остальных» как было на диске, то объекты,
 * поправленные на загрузке, и объекты, созданные приложением (их режим считает
 * as_sdcardfs_file/as_sdcardfs_dir в src/hook_libc.cpp, а ACL пишет acl_build с
 * ACL_OTHER=0), выглядели бы по-разному. Хуже того, «остальные» получили бы
 * доступ В ОБХОД записи для 9997 — то есть шире, чем было на Android 10, где
 * sdcardfs пускал только процессы из группы 9997.
 *
 * Ровно поэтому же вызовы ниже передают в acl_apply уже обнулённые «остальные»,
 * и `mode & S_IRWXO` внутри acl_apply даёт 0. Сама acl_apply остаётся точным
 * повтором vold::SetDefaultAcl (vold-16/Utils.cpp:142) — vold всегда зовёт её с
 * режимом 0770, где «остальные» и так нулевые.
 */

/* Каталог: владелец сохраняется, группе — rwx. */
static mode_t dir_mode(mode_t m) { return (m & S_IRWXU) | S_IRWXG; }

/* Файл: владельцу — как было, группе — rw, плюс x, если он был исполняемым. */
static mode_t file_mode(mode_t m) {
    return (m & S_IRWXU) | (m & S_IRWXG) | S_IRGRP | S_IWGRP;
}

/* Корень тома: группе только r-x — его надо пройти, но не менять. */
static mode_t traverse_mode(mode_t m) {
    return (m & S_IRWXU) | (m & S_IRWXG) | S_IRGRP | S_IXGRP;
}

static int fix_dir(const char *path, mode_t m, int traverse_only) {
    const mode_t want = traverse_only ? traverse_mode(m) : dir_mode(m);
    int bad = 0;

    if (acl_apply(path, XATTR_ACL_ACCESS, want) != 0) bad = 1;
    if (acl_apply(path, XATTR_ACL_DEFAULT, want) != 0) bad = 1;

    if (bad) {
        fprintf(stderr, "storage-fix: %s: %s\n", path, strerror(errno));
        stat_errors++;
        return -1;
    }
    stat_dirs++;
    return 0;
}

static int fix_file(const char *path, mode_t m) {
    if (acl_apply(path, XATTR_ACL_ACCESS, file_mode(m)) != 0) {
        fprintf(stderr, "storage-fix: %s: %s\n", path, strerror(errno));
        stat_errors++;
        return -1;
    }
    stat_files++;
    return 0;
}

static void walk(const char *dir) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        fprintf(stderr, "storage-fix: %s: %s\n", dir, strerror(errno));
        stat_errors++;
        return;
    }

    const size_t base = strlen(dir);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        char *child = malloc(base + strlen(de->d_name) + 2);
        if (child == NULL) {
            stat_errors++;
            continue;
        }
        sprintf(child, "%s/%s", dir, de->d_name);

        struct stat st;
        if (lstat(child, &st) != 0) {
            fprintf(stderr, "storage-fix: %s: %s\n", child, strerror(errno));
            stat_errors++;
        } else if (S_ISLNK(st.st_mode)) {
            stat_skipped++; /* на симлинк ACL не поставить, и он не нужен */
        } else if (S_ISDIR(st.st_mode)) {
            fix_dir(child, st.st_mode, 0);
            walk(child);
        } else if (S_ISREG(st.st_mode)) {
            fix_file(child, st.st_mode);
        } else {
            stat_skipped++;
        }
        free(child);
    }
    closedir(d);
}

/* ==========================================================================
 * Режим --check
 * ========================================================================== */

static void perm_str(uint16_t p, char out[4]) {
    out[0] = (p & 4) ? 'r' : '-';
    out[1] = (p & 2) ? 'w' : '-';
    out[2] = (p & 1) ? 'x' : '-';
    out[3] = '\0';
}

static int check_paths(int argc, char **argv, int first) {
    int bad = 0;

    for (int i = first; i < argc; i++) {
        struct stat st;
        if (lstat(argv[i], &st) != 0) {
            printf("ОШИБКА %s: %s\n", argv[i], strerror(errno));
            bad = 1;
            continue;
        }

        const int is_dir = S_ISDIR(st.st_mode);
        uint16_t a = 0, d = 0;
        const int ok_a = acl_allows_everybody(argv[i], XATTR_ACL_ACCESS, &a);
        const int ok_d = !is_dir || acl_allows_everybody(argv[i], XATTR_ACL_DEFAULT, &d);

        char sa[4], sd[4];
        perm_str(a, sa);
        perm_str(d, sd);

        printf("%s %s  access=%s default=%s\n", (ok_a && ok_d) ? "ОК  " : "НЕТ ",
               argv[i], ok_a ? sa : "нет 9997", is_dir ? (ok_d ? sd : "нет 9997") : "—");

        if (!ok_a || !ok_d) bad = 1;
    }

    return bad;
}

/* ==========================================================================
 * Режим --guard
 * ========================================================================== */

/*
 * Корень, за которым следит сторож, и файл замка. Переопределяются только при
 * сборке стенда на хосте (-DGUARD_ROOT=...): на устройстве это всегда /data/media
 * и /data/adb. Наличие этих двух #ifndef — единственное, что отличает проверку
 * на хосте от боевой сборки.
 */
#ifndef GUARD_ROOT
#define GUARD_ROOT "/data/media"
#endif
#ifndef GUARD_LOCK
#define GUARD_LOCK "/data/adb/sdcardfs_restore.guard.lock"
#endif

#define GUARD_TICK 10000 /* мс между пересмотрами списка */
#define GUARD_DEPTH 4    /* глубже каталогов пакетов сторож не идёт */

#define GUARD_MASK \
    (IN_ATTRIB | IN_CREATE | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR)

struct slot {
    int wd;
    char *path;
};

static struct slot *g_slots;
static size_t g_nslots, g_cslots;
static unsigned long g_fixes;

struct comp {
    const char *p;
    size_t n;
};

static void guard_log(const char *fmt, ...) {
    char ts[32] = "?";
    time_t now = time(NULL);
    struct tm tm;

    if (localtime_r(&now, &tm) != NULL) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(stderr, "[%s] сторож: ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/*
 * Разбирает путь на компоненты после /data/media.
 * Возвращает их число (0 — сам /data/media) или -1, если путь не под ним
 * либо глубже, чем нас интересует.
 */
static int media_split(const char *path, struct comp *out, int maxc) {
    const size_t rn = sizeof(GUARD_ROOT) - 1;
    if (strncmp(path, GUARD_ROOT, rn) != 0) return -1;

    const char *p = path + rn;
    int n = 0;

    while (*p == '/') {
        p++;
        const char *s = p;
        while (*p != '\0' && *p != '/') p++;
        if (p == s) break;

        if (n < maxc) {
            out[n].p = s;
            out[n].n = (size_t)(p - s);
        }
        n++;
        if (n > maxc) return -1;
    }

    return n;
}

static int comp_eq(const struct comp *c, const char *s) {
    const size_t n = strlen(s);
    return c->n == n && memcmp(c->p, s, n) == 0;
}

static int comp_digits(const struct comp *c) {
    if (c->n == 0) return 0;
    for (size_t i = 0; i < c->n; i++) {
        if (c->p[i] < '0' || c->p[i] > '9') return 0;
    }
    return 1;
}

/*
 * Нужен ли сторож на этом каталоге. Список закрытый и повторяет места, где
 * vold зовёт SetDefaultAcl, плюс /data/media — чтобы поймать появление каталога
 * пользователя. Глубже каталогов пакетов сторож не идёт: там каталоги создают
 * приложения, а их правки делает хук libc.
 */
static int guard_want(const char *path) {
    struct comp c[6];
    const int n = media_split(path, c, 5);

    if (n < 0) return 0;
    if (n == 0) return 1;              /* /data/media */
    if (n > GUARD_DEPTH) return 0;
    if (!comp_digits(&c[0])) return 0; /* /data/media/<user> */
    if (n == 1) return 1;
    if (!comp_eq(&c[1], "Android")) return 0;
    if (n == 2) return 1;
    if (!comp_eq(&c[2], "data") && !comp_eq(&c[2], "obb") && !comp_eq(&c[2], "media")) return 0;
    return 1;                          /* сам каталог или пакет внутри него */
}

static const char *slot_path(int wd) {
    for (size_t i = 0; i < g_nslots; i++) {
        if (g_slots[i].wd == wd) return g_slots[i].path;
    }
    return NULL;
}

static int slot_has(const char *path) {
    for (size_t i = 0; i < g_nslots; i++) {
        if (strcmp(g_slots[i].path, path) == 0) return 1;
    }
    return 0;
}

static void slot_add(int wd, const char *path) {
    if (g_nslots == g_cslots) {
        const size_t nc = g_cslots ? g_cslots * 2 : 32;
        struct slot *ns = realloc(g_slots, nc * sizeof(*ns));
        if (ns == NULL) return;
        g_slots = ns;
        g_cslots = nc;
    }

    char *copy = strdup(path);
    if (copy == NULL) return;

    g_slots[g_nslots].wd = wd;
    g_slots[g_nslots].path = copy;
    g_nslots++;
}

static void slot_drop(int wd) {
    for (size_t i = 0; i < g_nslots; i++) {
        if (g_slots[i].wd == wd) {
            free(g_slots[i].path);
            g_slots[i] = g_slots[--g_nslots];
            return;
        }
    }
}

static void slot_reset(void) {
    for (size_t i = 0; i < g_nslots; i++) free(g_slots[i].path);
    g_nslots = 0;
}

static int guard_watch(int ifd, const char *path) {
    if (slot_has(path)) return 0;

    const int wd = inotify_add_watch(ifd, path, GUARD_MASK);
    if (wd < 0) return -1;

    /* Тот же inode мог быть уже под сторожем под другим путём (bind mount). */
    if (slot_path(wd) == NULL) slot_add(wd, path);
    return 0;
}

/*
 * Правка каталога по факту. Сначала читает ACL и пишет, только если записи 9997
 * нет: иначе собственные setxattr сторожа порождали бы новые IN_ATTRIB и цикл
 * событий не заканчивался бы никогда.
 */
static void guard_fix(const char *path, int traverse_only) {
    struct stat st;

    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    if (acl_ok(path, 1)) return;

    if (fix_dir(path, st.st_mode, traverse_only) == 0) {
        guard_log("вернул запись 9997: %s (правок за сеанс: %lu)", path, ++g_fixes);
    }
}

static char *path_join(const char *dir, const char *name) {
    const size_t a = strlen(dir), b = strlen(name);
    char *out = malloc(a + b + 2);
    if (out != NULL) sprintf(out, "%s/%s", dir, name);
    return out;
}

/*
 * Обходит каталог до глубины GUARD_DEPTH: ставит сторож на всё, что попадает в
 * список, и заодно проверяет ACL. Вызывается при старте, по IN_CREATE нового
 * каталога и раз в GUARD_TICK как страховка от потерянного события.
 */
static void guard_scan(int ifd, const char *path) {
    struct comp c[6];
    const int n = media_split(path, c, 5);
    if (n < 0 || !guard_want(path)) return;

    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return;

    guard_watch(ifd, path);

    /* Корень тома не трогаем: у него ACL ставит storage.sh с --traverse, а
       сторожить его нужно лишь для того, чтобы поймать появление <user>. */
    if (n >= 1) guard_fix(path, 0);
    if (n >= GUARD_DEPTH) return;

    DIR *d = opendir(path);
    if (d == NULL) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char *child = path_join(path, de->d_name);
        if (child != NULL) {
            guard_scan(ifd, child);
            free(child);
        }
    }
    closedir(d);
}

static void guard_event(int ifd, const struct inotify_event *ev) {
    const char *found = slot_path(ev->wd);
    char *path = found ? strdup(found) : NULL;

    /* IN_IGNORED приходит, когда ядро само сняло сторож: каталог удалён или
       заменён. Запись в таблице после этого недействительна. */
    if (ev->mask & IN_IGNORED) {
        slot_drop(ev->wd);
        free(path);
        return;
    }
    if (path == NULL) return;

    if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
        slot_drop(ev->wd);
        free(path);
        return;
    }

    if ((ev->mask & (IN_CREATE | IN_MOVED_TO)) != 0 && ev->len > 0 && ev->name[0] != '\0') {
        char *child = path_join(path, ev->name);
        if (child != NULL) {
            struct stat st;
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
                /* Новый каталог: права он унаследовал от родителя на момент
                   создания, а vold может дописать свой default-ACL следом —
                   поэтому и сторожим его, и правим сразу. */
                guard_scan(ifd, child);
            }
            free(child);
        }
    }

    /* Изменение метаданных самого каталога — так выглядит setxattr от vold. */
    if (ev->mask & IN_ATTRIB) guard_fix(path, 0);

    free(path);
}

/* Возвращает 0 — читать больше нечего, -1 — inotify сломался. */
static int guard_drain(int ifd) {
    char buf[8192] __attribute__((aligned(__alignof__(struct inotify_event))));

    for (;;) {
        const ssize_t len = read(ifd, buf, sizeof(buf));
        if (len < 0) {
            if (errno == EAGAIN || errno == EINTR) return 0;
            return -1;
        }
        if (len == 0) return 0;

        for (char *p = buf; p < buf + len;) {
            struct inotify_event *ev = (struct inotify_event *)(void *)p;
            guard_event(ifd, ev);
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
}

static int guard_main(void) {
    /*
     * Один сторож на устройство: storage.sh вызывается дважды за загрузку
     * (post-fs-data и service), и второй вызов не должен поднимать второго
     * сторожа. flock снимается ядром при выходе процесса, поэтому «залипшего»
     * замка после падения не остаётся.
     */
    const int lock = open(GUARD_LOCK, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "storage-fix: сторож уже работает\n");
        return 0;
    }

    guard_log("запущен, слежу за %s", GUARD_ROOT);

    int ifd = -1;

    for (;;) {
        if (ifd < 0) {
            ifd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
            if (ifd < 0) {
                guard_log("inotify_init: %s — повтор через 5 с", strerror(errno));
                sleep(5);
                continue;
            }
            /* Дескрипторы старых сторожей после пересоздания fd недействительны. */
            slot_reset();
            guard_scan(ifd, GUARD_ROOT);
            guard_log("под сторожем каталогов: %zu", g_nslots);
        }

        struct pollfd pfd;
        pfd.fd = ifd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        const int pr = poll(&pfd, 1, GUARD_TICK);
        if (pr < 0) {
            if (errno == EINTR) continue;
            guard_log("poll: %s — пересоздаю слежение", strerror(errno));
            close(ifd);
            ifd = -1;
            sleep(2);
            continue;
        }

        if (pr == 0) {
            guard_scan(ifd, GUARD_ROOT);
            continue;
        }

        if (guard_drain(ifd) != 0) {
            guard_log("чтение событий: %s — пересоздаю слежение", strerror(errno));
            close(ifd);
            ifd = -1;
            sleep(2);
        }
    }
}

/* ==========================================================================
 * main
 * ========================================================================== */

static int usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--traverse] <dir>...\n"
            "       %s --check <dir>...\n"
            "       %s --guard\n",
            argv0, argv0, argv0);
    return 2;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--guard") == 0) return guard_main();

    if (argc > 1 && strcmp(argv[1], "--check") == 0) {
        if (argc <= 2) return usage(argv[0]);
        return check_paths(argc, argv, 2);
    }

    int traverse_only = 0;
    int first = 1;

    if (argc > 1 && strcmp(argv[1], "--traverse") == 0) {
        traverse_only = 1;
        first = 2;
    }
    if (argc <= first) return usage(argv[0]);

    for (int i = first; i < argc; i++) {
        const char *root = argv[i];
        struct stat st;
        if (lstat(root, &st) != 0) {
            fprintf(stderr, "storage-fix: %s: %s\n", root, strerror(errno));
            stat_errors++;
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "storage-fix: %s: не каталог\n", root);
            stat_errors++;
            continue;
        }
        fix_dir(root, st.st_mode, traverse_only);
        if (!traverse_only) walk(root);
    }

    printf("storage-fix: каталогов=%lu файлов=%lu пропущено=%lu ошибок=%lu\n", stat_dirs,
           stat_files, stat_skipped, stat_errors);
    return stat_errors == 0 ? 0 : 1;
}
