/*
 * storage-fix — нормализация прав сырого дерева /data/media для ядер без sdcardfs.
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
 * Использование:
 *   storage-fix <каталог>...          — выдать группе 9997 rwx/rw рекурсивно
 *   storage-fix --traverse <каталог>  — только r-x на сам каталог (для корня
 *                                       тома: его нужно пройти, но не писать в него)
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#define XATTR_ACL_ACCESS "system.posix_acl_access"
#define XATTR_ACL_DEFAULT "system.posix_acl_default"

/* linux/include/uapi/linux/posix_acl_xattr.h */
#define POSIX_ACL_XATTR_VERSION 0x0002
#define ACL_USER_OBJ 0x01
#define ACL_GROUP_OBJ 0x04
#define ACL_GROUP 0x08
#define ACL_MASK 0x10
#define ACL_OTHER 0x20

/* android_filesystem_config.h: общая группа всех приложений одного профиля */
#define AID_EVERYBODY 9997

struct acl_entry {
    uint16_t e_tag;
    uint16_t e_perm;
    uint32_t e_id;
};

static unsigned long stat_dirs, stat_files, stat_skipped, stat_errors;

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

static void fix_dir(const char *path, mode_t m, int traverse_only) {
    const mode_t want = traverse_only ? traverse_mode(m) : dir_mode(m);
    int bad = 0;

    if (acl_apply(path, XATTR_ACL_ACCESS, want) != 0) bad = 1;
    if (acl_apply(path, XATTR_ACL_DEFAULT, want) != 0) bad = 1;

    if (bad) {
        fprintf(stderr, "storage-fix: %s: %s\n", path, strerror(errno));
        stat_errors++;
        return;
    }
    stat_dirs++;
}

static void fix_file(const char *path, mode_t m) {
    if (acl_apply(path, XATTR_ACL_ACCESS, file_mode(m)) != 0) {
        fprintf(stderr, "storage-fix: %s: %s\n", path, strerror(errno));
        stat_errors++;
        return;
    }
    stat_files++;
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

int main(int argc, char **argv) {
    int traverse_only = 0;
    int first = 1;

    if (argc > 1 && strcmp(argv[1], "--traverse") == 0) {
        traverse_only = 1;
        first = 2;
    }
    if (argc <= first) {
        fprintf(stderr, "usage: %s [--traverse] <dir>...\n", argv[0]);
        return 2;
    }

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
