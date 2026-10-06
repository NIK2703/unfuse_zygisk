/*
 * vold-noacl — сделать vold::SetDefaultAcl() пустышкой в работающем vold.
 *
 * ------------------------------------------------------------------ зачем
 *
 * vold при подготовке CE-хранилища пользователя вызывает (vold-16/FsCrypt.cpp:1027)
 *
 *     SetDefaultAcl(media_ce_path, 02770, AID_MEDIA_RW, AID_MEDIA_RW, {AID_MEDIA_RW})
 *
 * а тот ставит на каталог default-ACL, в котором additionalGid превращается в
 * ИМЕНОВАННУЮ запись для группы 1023 (media_rw). Модуль же выдаёт доступ к
 * общему хранилищу именованной записью для 9997 (AID_EVERYBODY). Что унаследуют
 * новые каталоги, решает тот, кто записал default-ACL последним, — а vold пишет
 * позже (при разблокировке пользователя, уже после post-fs-data). В итоге
 * каталог, созданный не приложением (vold, MTP, root-демон), наследует запись
 * для 1023, и приложения его не видят.
 *
 * ------------------------------------------------------------- что патчим
 *
 * vold-16/Utils.cpp:142:
 *
 *     status_t SetDefaultAcl(const std::string& path, mode_t mode, uid_t uid,
 *                            gid_t gid, std::vector<gid_t> additionalGids) {
 *         if (IsSdcardfsUsed()) {
 *             return OK;            // «sdcardfs magically takes care of this»
 *         }
 *         ... строит ACL и зовёт setxattr(..., XATTR_NAME_POSIX_ACL_DEFAULT, ...)
 *     }
 *
 * IsSdcardfsUsed() (vold-16/Utils.cpp:1112) — это
 *
 *     IsFilesystemSupported("sdcardfs") &&
 *     base::GetBoolProperty("external_storage.sdcardfs.enabled", true)
 *
 * На этой прошивке sdcardfs в /proc/filesystems ЕСТЬ (первая половина истинна),
 * а свойство выставлено в 0 в /vendor/build.prop. Поэтому проверка даёт false, и
 * функция честно пишет default-ACL.
 *
 * Дизассемблер /system/bin/vold (arm64, Android 16, marble;
 * md5 2319c26fb4c5ccd1492b16fccc895cc5) — компилятор оставил по одной tbz на
 * каждую половину проверки:
 *
 *     0x54e94: f2ee6cc8   movk  x8, #0x7366, lsl #48    ┐
 *     0x54e98: f80213e8   stur  x8, [sp, #0x21]         │ якорь: эта
 *     0x54e9c: 940016d0   bl    IsFilesystemSupported   │ тройка слов
 *     0x54ea0: 36000340   tbz   w0, #0, 0x54f08          ┘ уникальна в файле
 *     0x54ea4: ...        (сборка строки свойства, GetBoolProperty)
 *     0x54efc: 36000077   tbz   w23, #0, 0x54f08
 *     0x54f00: 2a1f03f4   mov   w20, wzr                 <- «return OK»
 *     0x54f04: 14000045   b     0x55018                  <- эпилог, mov w0, w20
 *
 * Обе tbz ведут в одно место (0x54f08) — блок, который строит ACL. Замена первой
 * на безусловный b в «return OK» делает функцию пустышкой независимо и от
 * /proc/filesystems, и от свойства.
 *
 * Так vold и ведёт себя на устройствах с sdcardfs: там он этих ACL не ставит
 * вовсе. То есть патч не изобретает поведение, а возвращает ветку, которую AOSP
 * и так предусмотрел, — поэтому он же закрывает и два других вызова
 * SetDefaultAcl (каталог приложения, Utils.cpp:406; Android/obb, Utils.cpp:1889):
 * они попадают под ту же пустышку ровно так же, как попадали бы под sdcardfs.
 *
 * --------------------------------------------------- почему живой процесс
 *
 * Запись идёт через /proc/<pid>/mem. Ядро обслуживает её тем же путём, что
 * ptrace POKEDATA: пишет с FOLL_FORCE, ломая COW для приватной копии страницы
 * текста, и само сбрасывает i-кэш для отображений с VM_EXEC. Процесс при этом
 * НЕ останавливается — ни PTRACE_ATTACH, ни SIGSTOP, — то есть нет риска
 * оставить vold замороженным, если что-то пойдёт не так. Именно поэтому здесь
 * /proc/<pid>/mem, а не ptrace.
 *
 * Утилита ничего не пишет, пока не сверит найденное место с ожидаемым: находит
 * уникальный якорь, проверяет, что за ним действительно tbz, и что найденный
 * блок «return OK» — это именно `mov w20, wzr` + безусловный переход. Если vold
 * другой сборки, якоря не будет, и утилита откажется работать (код 2), а модуль
 * вернётся к прежним повторным проходам ACL.
 *
 * --------------------------------------------------------------- запуск
 *
 *   vold-noacl [--dry-run] [--wait СЕК] [--quiet]
 *
 *     --dry-run   только проверить и доложить, ничего не писать
 *     --wait СЕК  ждать появления vold до СЕК секунд (по умолчанию 0)
 *     --quiet     печатать только ошибки
 *
 * Коды возврата: 0 — патч стоит (или уже стоял); 1 — ошибка запуска или
 * ввода-вывода; 2 — место не найдено либо байты не совпали, ничего не изменено.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VOLD_EXE "/system/bin/vold"

/* Якорь: три слова перед охранником. Уникальны во всём /system/bin/vold. */
#define ANCHOR_W0 0xf2ee6cc8u /* movk x8, #0x7366, lsl #48 */
#define ANCHOR_W1 0xf80213e8u /* stur x8, [sp, #0x21]       */
#define ANCHOR_W2 0x940016d0u /* bl   IsFilesystemSupported */

/* Начало блока «return OK»: mov w20, wzr */
#define RET_W0 0x2a1f03f4u

/* Насколько далеко после охранника искать «return OK» (в байтах). */
#define RET_SEARCH 192

/* Сколько байт читать из /proc/<pid>/mem за раз. */
#define CHUNK 65536

/* ------------------------------------------------------------------ вывод */

static int g_quiet;

static void say(const char *fmt, ...)
{
    va_list ap;

    if (g_quiet)
        return;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

static void warn(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------- инструкции */

/*
 * Разбор слов AArch64 (только то, что нужно):
 *
 *   B   : 000101 imm26                          — безусловный переход
 *   TBZ : b5=0, 011011, бит[23:19], imm14[18:5], Rt[4:0]
 */
static int is_b(uint32_t w)
{
    return (w >> 26) == 0x05u;
}

static uint64_t b_target(uint64_t pc, uint32_t w)
{
    int32_t imm = (int32_t)(w & 0x03ffffffu);

    if (imm & 0x02000000) /* знаковое расширение 26 бит */
        imm |= (int32_t)0xfc000000;
    return (uint64_t)((int64_t)pc + ((int64_t)imm << 2));
}

static uint32_t b_encode(uint64_t pc, uint64_t target)
{
    int64_t imm = ((int64_t)target - (int64_t)pc) >> 2;

    return 0x14000000u | ((uint32_t)imm & 0x03ffffffu);
}

static int is_tbz_bit0(uint32_t w)
{
    return (w >> 24) == 0x36u && ((w >> 19) & 0x1fu) == 0;
}

static uint64_t tbz_target(uint64_t pc, uint32_t w)
{
    int64_t imm = (int64_t)((w >> 5) & 0x3fffu);

    return (uint64_t)((int64_t)pc + (imm << 2));
}

/* ------------------------------------------------------------ /proc/<pid> */

/*
 * pid процесса, запущенного из /system/bin/vold, либо -1.
 *
 * Смотрим /proc/<pid>/exe, а не comm: comm обрезается до 15 символов и его
 * может выставить любой процесс, а exe — нет.
 */
static pid_t find_vold(void)
{
    DIR *d;
    struct dirent *e;
    pid_t found = -1;

    d = opendir("/proc");
    if (!d)
        return -1;

    while ((e = readdir(d)) != NULL) {
        char path[64];
        char target[256];
        ssize_t n;

        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;

        snprintf(path, sizeof path, "/proc/%s/exe", e->d_name);
        n = readlink(path, target, sizeof target - 1);
        if (n <= 0)
            continue;
        target[n] = '\0';

        if (strcmp(target, VOLD_EXE) == 0) {
            found = (pid_t)atoi(e->d_name);
            break;
        }
    }
    closedir(d);
    return found;
}

struct maps_info {
    uint64_t base;    /* адрес, по которому отображён файл со смещения 0 */
    uint64_t exec_lo; /* границы исполняемого сегмента */
    uint64_t exec_hi;
    int have_base;
    int have_exec;
};

/*
 * Путь в строке /proc/<pid>/maps — всё, что начинается с первого «/»: ни одно
 * из полей до него (диапазон, права, смещение, dev, inode) косой черты не
 * содержит.
 */
static int map_is_vold(const char *line)
{
    const char *p = strchr(line, '/');

    if (!p)
        return 0;
    if (strncmp(p, VOLD_EXE, sizeof VOLD_EXE - 1) != 0)
        return 0;
    p += sizeof VOLD_EXE - 1;
    while (*p == ' ' || *p == '\t') /* хвост "(deleted)" допустим */
        p++;
    return *p == '\0' || *p == '\n' || *p == '(';
}

static int read_maps(pid_t pid, struct maps_info *mi)
{
    char path[64];
    FILE *f;
    char line[512];

    memset(mi, 0, sizeof *mi);

    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    f = fopen(path, "r");
    if (!f)
        return -1;

    while (fgets(line, sizeof line, f) != NULL) {
        unsigned long long start, end, off;
        char perms[8];

        if (!map_is_vold(line))
            continue;
        if (sscanf(line, "%llx-%llx %7s %llx", &start, &end, perms, &off) != 4)
            continue;

        if (off == 0) {
            mi->base = start;
            mi->have_base = 1;
        }
        if (strchr(perms, 'x') != NULL) {
            if (!mi->have_exec || start < mi->exec_lo)
                mi->exec_lo = start;
            if (!mi->have_exec || end > mi->exec_hi)
                mi->exec_hi = end;
            mi->have_exec = 1;
        }
    }
    fclose(f);
    return 0;
}

/* ---------------------------------------------------------------- память */

static int rd(int fd, uint64_t addr, void *buf, size_t len)
{
    return pread(fd, buf, len, (off_t)addr) == (ssize_t)len ? 0 : -1;
}

static int wr(int fd, uint64_t addr, const void *buf, size_t len)
{
    return pwrite(fd, buf, len, (off_t)addr) == (ssize_t)len ? 0 : -1;
}

static int rd32(int fd, uint64_t addr, uint32_t *out)
{
    return rd(fd, addr, out, sizeof *out);
}

/*
 * Ищет якорь в исполняемом сегменте. Возвращает адрес его первого слова
 * (ANCHOR_W0) либо 0.
 *
 * Сегмент читается целиком (у vold это ~845 КБ): так не нужно возиться со
 * стыками кусков, а непрочитанный кусок просто обнуляется — нулевое слово
 * якорю не равно, ложного совпадения не будет.
 */
static uint64_t find_anchor(int fd, uint64_t lo, uint64_t hi)
{
    size_t total;
    unsigned char *buf;
    size_t got = 0;
    uint64_t found = 0;
    size_t i;

    if (hi <= lo || hi - lo > (64u << 20))
        return 0;

    total = (size_t)(hi - lo);
    buf = malloc(total);
    if (!buf)
        return 0;

    while (got < total) {
        size_t want = total - got;

        if (want > CHUNK)
            want = CHUNK;
        if (rd(fd, lo + got, buf + got, want) != 0)
            memset(buf + got, 0, want);
        got += want;
    }

    for (i = 0; i + 12 <= total; i += 4) {
        uint32_t a, b, c;

        memcpy(&a, buf + i, 4);
        memcpy(&b, buf + i + 4, 4);
        memcpy(&c, buf + i + 8, 4);
        if (a == ANCHOR_W0 && b == ANCHOR_W1 && c == ANCHOR_W2) {
            found = lo + i;
            break;
        }
    }

    free(buf);
    return found;
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    int dry = 0;
    int wait_sec = 0;
    int waited;
    pid_t pid;
    struct maps_info mi;
    char mpath[64];
    int fd;
    uint64_t anchor, guard, ret = 0;
    uint32_t w, x;
    int rc = 2;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry = 1;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            g_quiet = 1;
        } else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc) {
            wait_sec = atoi(argv[++i]);
            if (wait_sec < 0)
                wait_sec = 0;
        } else {
            warn("vold-noacl: неизвестный аргумент: %s", argv[i]);
            return 1;
        }
    }

    pid = find_vold();
    for (waited = 0; pid < 0 && waited < wait_sec; waited++) {
        sleep(1);
        pid = find_vold();
    }
    if (pid < 0) {
        warn("vold-noacl: процесс vold не найден");
        return 2;
    }

    if (read_maps(pid, &mi) != 0) {
        warn("vold-noacl: не читается /proc/%d/maps: %s", pid, strerror(errno));
        return 1;
    }
    if (!mi.have_base || !mi.have_exec) {
        warn("vold-noacl: в maps процесса %d нет сегментов %s", pid, VOLD_EXE);
        return 2;
    }

    snprintf(mpath, sizeof mpath, "/proc/%d/mem", pid);
    fd = open(mpath, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        warn("vold-noacl: %s: %s", mpath, strerror(errno));
        return 1;
    }

    anchor = find_anchor(fd, mi.exec_lo, mi.exec_hi);
    if (anchor == 0) {
        warn("vold-noacl: якорь SetDefaultAcl не найден — vold другой сборки");
        goto out;
    }
    guard = anchor + 12;

    if (rd32(fd, guard, &w) != 0) {
        warn("vold-noacl: не читается слово по адресу 0x%" PRIx64, guard);
        rc = 1;
        goto out;
    }

    /* Блок «return OK»: mov w20, wzr, сразу за ним — безусловный переход. */
    for (uint64_t a = guard + 4; a + 8 <= guard + 4 + RET_SEARCH; a += 4) {
        uint32_t y;

        if (rd32(fd, a, &x) != 0 || x != RET_W0)
            continue;
        if (rd32(fd, a + 4, &y) != 0 || !is_b(y))
            continue;
        if (b_target(a + 4, y) <= a + 4)
            continue; /* эпилог обязан быть дальше */
        ret = a;
        break;
    }
    if (ret == 0) {
        warn("vold-noacl: не найден блок «return OK» после охранника");
        goto out;
    }

    /* Уже пропатчено? */
    if (is_b(w)) {
        if (b_target(guard, w) == ret) {
            say("vold-noacl: патч уже стоит (pid=%d, +0x%" PRIx64 ")",
                pid, guard - mi.base);
            rc = 0;
        } else {
            warn("vold-noacl: на месте охранника чужой переход — отказ");
        }
        goto out;
    }

    if (!is_tbz_bit0(w)) {
        warn("vold-noacl: на месте охранника не tbz (0x%08" PRIx32 ") — отказ", w);
        goto out;
    }
    if (tbz_target(guard, w) <= guard) {
        warn("vold-noacl: охранник ветвится назад — отказ");
        goto out;
    }

    say("vold-noacl: pid=%d база=0x%" PRIx64 ", охранник +0x%" PRIx64
        " -> b +0x%" PRIx64,
        pid, mi.base, guard - mi.base, ret - mi.base);

    if (dry) {
        say("vold-noacl: --dry-run, ничего не записано");
        rc = 0;
        goto out;
    }

    x = b_encode(guard, ret);
    if (wr(fd, guard, &x, sizeof x) != 0) {
        warn("vold-noacl: запись не удалась: %s", strerror(errno));
        rc = 1;
        goto out;
    }

    if (rd32(fd, guard, &x) != 0 || x != b_encode(guard, ret)) {
        warn("vold-noacl: запись не подтвердилась при чтении обратно");
        rc = 1;
        goto out;
    }

    say("vold-noacl: патч поставлен");
    rc = 0;

out:
    close(fd);
    return rc;
}
