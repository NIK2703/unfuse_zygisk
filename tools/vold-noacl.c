/*
 * vold-noacl — сделать vold::SetDefaultAcl() пустышкой в работающем vold,
 *              не привязываясь к сборке этого vold.
 *
 * ================================================================== зачем
 *
 * При подготовке CE-хранилища пользователя vold вызывает
 * (vold-16/FsCrypt.cpp:1027)
 *
 *     SetDefaultAcl(media_ce_path, 02770, AID_MEDIA_RW, AID_MEDIA_RW, {AID_MEDIA_RW})
 *
 * и тот ставит на /data/media/<user> default-ACL, в котором additionalGid
 * превращается в ИМЕНОВАННУЮ запись для группы 1023 (media_rw). Модуль же
 * выдаёт доступ к общему хранилищу именованной записью для 9997
 * (AID_EVERYBODY). Что унаследуют новые каталоги, решает тот, кто записал
 * default-ACL последним, — а vold пишет позже (при подготовке CE, уже после
 * post-fs-data). В итоге каталог, созданный не приложением (vold, MTP,
 * root-демон), наследует запись для 1023, и приложения его не видят.
 *
 * SetDefaultAcl зовётся из трёх мест, и все три портят картину:
 *
 *   FsCrypt.cpp:1027   /data/media/<user>              запись для 1023  <- главная
 *   Utils.cpp:406      Android/{data,obb,media}/<pkg>  запись для uid пакета
 *   Utils.cpp:1889     Android/obb                     GROUP_OBJ = AID_EXT_OBB_RW
 *
 * ================================================================ что патчим
 *
 * В AOSP у SetDefaultAcl есть готовая ветка «ничего не делать»
 * (vold-16/Utils.cpp:142):
 *
 *     status_t SetDefaultAcl(...) {
 *         if (IsSdcardfsUsed()) {
 *             return OK;            // «sdcardfs magically takes care of this»
 *         }
 *         ... строит ACL и зовёт setxattr(path, XATTR_NAME_POSIX_ACL_DEFAULT, ...)
 *     }
 *
 * На устройствах с sdcardfs vold этих ACL не ставит вовсе — то есть модуль не
 * изобретает поведение, а возвращает ветку, которую AOSP и так предусмотрел.
 *
 * IsSdcardfsUsed() — это `IsFilesystemSupported("sdcardfs") &&
 * GetBoolProperty("external_storage.sdcardfs.enabled", true)`. На этой прошивке
 * sdcardfs в /proc/filesystems ЕСТЬ, а свойство выставлено в 0 в
 * /vendor/build.prop, поэтому проверка даёт false. Включить свойство обратно
 * нельзя: им гейтится полтора десятка других мест vold.
 *
 * Первая версия этого патча (v2.9.0) переписывала саму ветку: в
 * /system/bin/vold по трёхсловному якорю (f2ee6cc8 f80213e8 940016d0) искалась
 * инструкция `tbz w0, #0, <блок ACL>` и заменялась на `b <return OK>`. Это
 * работало, но было привязано к конкретной сборке: на другом vold якорь не
 * находится, и патч просто отказывался вставать.
 *
 * ------------------------------------------------------ почему это надёжнее
 *
 * В SetDefaultAcl вся работа сводится к одному вызову setxattr. Проверено по
 * исходникам: во всём vold-16 setxattr встречается РОВНО ОДИН раз —
 * Utils.cpp:195, внутри SetDefaultAcl (getxattr нет вовсе). Значит,
 *
 *     «setxattr в vold ничего не делает»  ==  «SetDefaultAcl возвращает OK,
 *                                              не записав ACL»
 *
 * то есть в точности то же самое, что делал патч v2.9.0, — только привязки к
 * сборке нет: setxattr — импортируемый символ libc, и его адрес берётся ИЗ
 * ТАБЛИЦ САМОГО vold, а не из подписи байтов.
 *
 * ------------------------------------------------------------- как ищется
 *
 * 1. База загрузки: /proc/<pid>/maps, первая запись с offset=0 (PIE-образ
 *    отображён целиком, поэтому все адреса в ELF — смещения от этой базы).
 *
 * 2. Смещение GOT-слота setxattr: в PT_DYNAMIC берётся DT_JMPREL
 *    (таблица .rela.plt), в ней ищется R_AARCH64_JUMP_SLOT, чей индекс символа
 *    в .dynsym указывает на UNDEF-символ с именем "setxattr". r_offset этой
 *    релокации и есть виртуальный адрес GOT-слота.
 *
 * 3. Трамплин PLT. Сначала одним проходом по исполняемым PT_LOAD собираются
 *    все записи канонического вида
 *
 *        adrp x16, <страница>
 *        ldr  x17, [x16, #<imm12*8>]     <- обязан попасть ровно в GOT-слот
 *        add  x16, x16, #<imm12*8>
 *        br   x17
 *
 *    Совпадение адреса из `ldr` с r_offset из релокации — это самопроверка:
 *    она доказывает, что найденный трамплин принадлежит именно setxattr.
 *    Индекс релокации для этого НЕ используется: у lld перед первым символом
 *    стоят две служебные записи, поэтому «rela.plt[i] -> plt + 16*(i+1)» даёт
 *    промах на одну запись (проверено на устройстве: setxattr — это
 *    rela.plt[185], а трамплин у него #187). Режим --selftest проверяет, что
 *    сопоставление взаимно однозначное для ВСЕХ символов сразу.
 *
 * 4. Повторный запуск (патч уже стоит). Патч затирает `ldr`, поэтому найти
 *    трамплин по GOT-слоту во второй раз нельзя. Но раскладка .plt линейна:
 *    для любой ДРУГОЙ пары «релокация i -> трамплин v» величина
 *
 *        C = v - 16 * i
 *
 *    одна и та же (это и есть смещение, на которое lld сдвинул нумерацию).
 *    C выводится из чужих пар, после чего адрес setxattr считается как
 *    C + 16 * i_setxattr. Так повторный запуск и --check работают, хотя
 *    собственный ldr символа уже затёрт. Именно эту величину v2.9.0
 *    ЗАШИВАЛА в код как «0x54ea0» — здесь она вычисляется из самого бинарника.
 *
 * 5. Патч: в трамплин пишутся 8 байт
 *
 *        mov w0, #0      ; 0x52800000  ->  setxattr «успешно» вернула 0
 *        ret             ; 0xd65f03c0
 *
 *    Восемь, а не шестнадцать: запись через /proc/<pid>/mem идёт memcpy в
 *    страницу, и восемь байт по выровненному адресу ложатся одним словом —
 *    то есть атомарно. Хвостовые `add`/`br` остаются на месте, но недостижимы:
 *    `ret` стоит раньше. Так исключено состояние, в котором поток, попавший в
 *    трамплин ровно во время записи, прыгнул бы по мусорному x17.
 *
 *    Возврат именно 0, а не -1: в FsCrypt.cpp:1027 результат проверяется —
 *    `if (ret != android::OK) return false;` — и ненулевой код сорвал бы всю
 *    подготовку хранилища пользователя.
 *
 * Страница .plt отображена из файла как r-x (RELRO покрывает только .got и
 * .got.plt, они идут позже), запись в неё через /proc/<pid>/mem вызывает COW:
 * правится приватная копия процесса, файл на диске и другие процессы не
 * затронуты. Ровно так же писал патч v2.9.0. Патч живёт только в памяти
 * процесса, поэтому после каждой перезагрузки его надо ставить заново — и на
 * свежем vold трамплин снова цел, так что первый запуск всегда идёт по п. 3.
 *
 * ============================================================== как позвать
 *
 *     vold-noacl [--wait СЕК] [--pid PID] [--check] [--dry-run]
 *                [--file ELF] [--selftest] [--quiet]
 *
 *     без флагов      найти vold, поставить патч (повторный запуск — no-op)
 *     --check         ничего не писать; 0 — патч на месте, 1 — нет
 *     --dry-run       найти и показать адреса, ничего не писать
 *     --file ELF      работать с файлом, а не с процессом (для проверок);
 *                     файл не меняется никогда, но --check и --dry-run
 *                     отвечают здесь ровно так же, как на живом процессе
 *     --selftest      с --file: сверить сопоставление релокаций и трамплинов
 *     --wait СЕК      ждать появления vold до СЕК секунд
 *     --pid PID       не искать vold, взять этот процесс
 *     --quiet         не печатать ничего, кроме ошибок
 *
 * Коды возврата:
 *     0  патч на месте (поставлен сейчас или стоял раньше)
 *     1  vold не найден / не запущен, либо --check увидел целый трамплин
 *     2  не удалось разобрать ELF, найти символ или трамплин
 *     3  не удалось записать
 *
 * Сборка: cc -std=c11 -Oz -o vold-noacl vold-noacl.c
 *         (на хосте проверялось clang -std=c11 -O2 -Wall -Wextra)
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Имя символа, который надо обезвредить. */
#define TARGET_SYM "setxattr"

/* Что пишем в трамплин: mov w0, #0 ; ret */
#define PATCH_MOV0  0x52800000u
#define PATCH_RET   0xd65f03c0u
static const uint32_t PATCH_WORDS[2] = { PATCH_MOV0, PATCH_RET };

#define EXIT_OK         0
#define EXIT_NO_VOLD    1
#define EXIT_NO_RESOLVE 2
#define EXIT_NO_WRITE   3

/* ------------------------------------------------------------------ вывод */

static bool g_quiet = false;

static void info(const char *fmt, ...) {
    if (g_quiet) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("vold-noacl: ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
    /* stdout может быть перенаправлен в журнал, stderr — нет: без сброса
     * строки перемешиваются. */
    fflush(stdout);
}

static void warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    /* Вся строка целиком в stderr. Если префикс писать в stderr, а текст через
     * vprintf — в stdout, то при `2>&1` в файл или канал строки расходятся:
     * stdout буферизуется блоками, stderr нет, и в журнале появляется
     * «vold-noacl: » отдельно от сообщения. */
    fputs("vold-noacl: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ------------------------------------------------------------------ мелкое */

static int read_all(const char *path, void *buf, size_t len, size_t *got) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, (char *)buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) break;
        off += (size_t)n;
    }
    close(fd);
    if (got) *got = off;
    return 0;
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* ------------------------------------------------------- источник ELF-образа
 *
 * Один и тот же разбор работает и по файлу (--file), и по живой памяти
 * процесса. Разница только в том, как читается «виртуальный адрес»:
 *
 *   файл     — VA переводится в смещение по таблице программных заголовков;
 *   процесс  — VA читается прямо из /proc/<pid>/mem как base + VA.
 */

typedef struct {
    bool      is_proc;
    int       fd;          /* файл или /proc/<pid>/mem */
    uint64_t  base;        /* база загрузки (только для процесса) */
    pid_t     pid;
    const char *label;     /* для сообщений */

    Elf64_Ehdr eh;
    Elf64_Phdr *ph;
    int         phnum;
} Src;

static int src_pread(Src *s, uint64_t va, void *buf, size_t len) {
    if (s->is_proc) {
        uint64_t addr = s->base + va;
        size_t off = 0;
        while (off < len) {
            ssize_t n = pread(s->fd, (char *)buf + off, len - off,
                              (off_t)(addr + off));
            if (n < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (n == 0) return -1;
            off += (size_t)n;
        }
        return 0;
    }

    /* Файл: VA -> смещение. Пробуем PT_LOAD, иначе считаем VA смещением
     * (так устроена шапка ELF и таблица программных заголовков). */
    uint64_t off = va;
    for (int i = 0; i < s->phnum; i++) {
        const Elf64_Phdr *p = &s->ph[i];
        if (p->p_type != PT_LOAD) continue;
        if (va >= p->p_vaddr && va < p->p_vaddr + p->p_filesz) {
            off = p->p_offset + (va - p->p_vaddr);
            break;
        }
    }
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(s->fd, (char *)buf + done, len - done,
                          (off_t)(off + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static void src_close(Src *s) {
    if (s->fd >= 0) close(s->fd);
    free(s->ph);
    s->fd = -1;
    s->ph = NULL;
}

/* Разбор шапки и программных заголовков. */
static int src_open_header(Src *s) {
    if (src_pread(s, 0, &s->eh, sizeof(s->eh)) != 0) {
        warn("%s: не читается шапка ELF", s->label);
        return -1;
    }
    if (memcmp(s->eh.e_ident, ELFMAG, SELFMAG) != 0) {
        warn("%s: это не ELF", s->label);
        return -1;
    }
    if (s->eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        s->eh.e_ident[EI_DATA] != ELFDATA2LSB) {
        warn("%s: поддержан только ELF64 little-endian (class=%u)",
             s->label, s->eh.e_ident[EI_CLASS]);
        return -1;
    }
    if (s->eh.e_machine != EM_AARCH64) {
        warn("%s: это не aarch64 (e_machine=%u) — 32-битный vold этим "
             "механизмом не поддержан", s->label, s->eh.e_machine);
        return -1;
    }
    if (s->eh.e_phnum == 0 || s->eh.e_phentsize != sizeof(Elf64_Phdr)) {
        warn("%s: непонятная таблица программных заголовков", s->label);
        return -1;
    }
    s->phnum = s->eh.e_phnum;
    s->ph = calloc((size_t)s->phnum, sizeof(*s->ph));
    if (!s->ph) return -1;
    if (src_pread(s, s->eh.e_phoff, s->ph,
                  (size_t)s->phnum * sizeof(*s->ph)) != 0) {
        warn("%s: не читаются программные заголовки", s->label);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------- PT_DYNAMIC */

typedef struct {
    uint64_t symtab, strtab, strsz;
    uint64_t jmprel, pltrelsz, pltrel, relaent;
    uint64_t reladyn, relasz;   /* нужны, чтобы заметить «взятие адреса» */
} Dyn;

static int read_dynamic(Src *s, Dyn *d) {
    memset(d, 0, sizeof(*d));
    uint64_t va = 0, sz = 0;
    for (int i = 0; i < s->phnum; i++) {
        if (s->ph[i].p_type == PT_DYNAMIC) {
            va = s->ph[i].p_vaddr;
            sz = s->ph[i].p_filesz;
            break;
        }
    }
    if (!va) {
        warn("%s: нет PT_DYNAMIC", s->label);
        return -1;
    }
    if (sz > 4096) sz = 4096;
    Elf64_Dyn *dyn = calloc(sz / sizeof(Elf64_Dyn) + 1, sizeof(Elf64_Dyn));
    if (!dyn) return -1;
    if (src_pread(s, va, dyn, sz) != 0) {
        warn("%s: не читается PT_DYNAMIC", s->label);
        free(dyn);
        return -1;
    }
    for (size_t i = 0; i < sz / sizeof(Elf64_Dyn); i++) {
        switch (dyn[i].d_tag) {
        case DT_NULL: goto done;
        case DT_SYMTAB:  d->symtab = dyn[i].d_un.d_ptr; break;
        case DT_STRTAB:  d->strtab = dyn[i].d_un.d_ptr; break;
        case DT_STRSZ:   d->strsz  = dyn[i].d_un.d_val; break;
        case DT_JMPREL:  d->jmprel = dyn[i].d_un.d_ptr; break;
        case DT_PLTRELSZ: d->pltrelsz = dyn[i].d_un.d_val; break;
        case DT_PLTREL:  d->pltrel  = dyn[i].d_un.d_val; break;
        case DT_RELA:    d->reladyn = dyn[i].d_un.d_ptr; break;
        case DT_RELASZ:  d->relasz  = dyn[i].d_un.d_val; break;
        case DT_RELAENT: d->relaent = dyn[i].d_un.d_val; break;
        default: break;
        }
    }
done:
    free(dyn);
    if (!d->symtab || !d->strtab || !d->jmprel) {
        warn("%s: в PT_DYNAMIC нет SYMTAB/STRTAB/JMPREL", s->label);
        return -1;
    }
    if (d->pltrel != DT_RELA) {
        warn("%s: DT_PLTREL=%llu, ожидался DT_RELA", s->label,
             (unsigned long long)d->pltrel);
        return -1;
    }
    if (d->relaent && d->relaent != sizeof(Elf64_Rela)) {
        warn("%s: DT_RELAENT=%llu", s->label, (unsigned long long)d->relaent);
        return -1;
    }
    return 0;
}

static int load_relocs(Src *s, const Dyn *d, Elf64_Rela **out, size_t *nout) {
    size_t n = d->pltrelsz / sizeof(Elf64_Rela);
    if (n == 0 || n > 200000) {
        warn("%s: непонятный размер .rela.plt (%zu)", s->label, n);
        return -1;
    }
    Elf64_Rela *rel = calloc(n, sizeof(Elf64_Rela));
    if (!rel) return -1;
    if (src_pread(s, d->jmprel, rel, n * sizeof(Elf64_Rela)) != 0) {
        warn("%s: не читается .rela.plt", s->label);
        free(rel);
        return -1;
    }
    *out = rel;
    *nout = n;
    return 0;
}

static int sym_name(Src *s, const Dyn *d, uint32_t idx, char *out, size_t outlen) {
    Elf64_Sym sym;
    uint64_t va = d->symtab + (uint64_t)idx * sizeof(Elf64_Sym);
    if (src_pread(s, va, &sym, sizeof(sym)) != 0) return -1;
    if (sym.st_name == 0 || sym.st_name >= d->strsz) return -1;
    char *nm = calloc(1, 512);
    if (!nm) return -1;
    uint64_t off = sym.st_name;
    size_t n = d->strsz - off;
    if (n > 511) n = 511;
    if (src_pread(s, d->strtab + off, nm, n) != 0) {
        free(nm);
        return -1;
    }
    nm[511] = '\0';
    snprintf(out, outlen, "%s", nm);
    free(nm);
    return 0;
}

/* ---------------------------------------------------- декодирование трамплина
 *
 * Канонический трамплин AArch64:
 *
 *     30 00 00 d0   adrp x16, <page>
 *     11 5a 41 f9   ldr  x17, [x16, #imm12*8]
 *     10 c2 0a 91   add  x16, x16, #imm12*8
 *     20 02 1f d6   br   x17
 *
 * Возвращает адрес, откуда читает `ldr`. Проверка формы намеренно строгая
 * (все четыре инструкции, регистры x16/x17) — чтобы исключить ложные
 * совпадения при сканировании.
 */
static bool decode_stub(const uint8_t *p, uint64_t va, uint64_t *ldr_target) {
    uint32_t w0, w1, w2, w3;
    memcpy(&w0, p + 0, 4);
    memcpy(&w1, p + 4, 4);
    memcpy(&w2, p + 8, 4);
    memcpy(&w3, p + 12, 4);

    if ((w0 & 0x9f000000u) != 0x90000000u) return false;  /* adrp */
    if ((w0 & 0x1fu) != 16u) return false;                /* Rd = x16 */
    if ((w1 & 0xffc00000u) != 0xf9400000u) return false;  /* ldr imm64 */
    if (((w1 >> 5) & 0x1fu) != 16u || (w1 & 0x1fu) != 17u) return false;
    if ((w2 & 0xffc00000u) != 0x91000000u) return false;  /* add imm */
    if (((w2 >> 5) & 0x1fu) != 16u || (w2 & 0x1fu) != 16u) return false;
    if (w3 != 0xd61f0220u) return false;                  /* br x17 */

    uint32_t immlo = (w0 >> 29) & 0x3u;
    uint32_t immhi = (w0 >> 5) & 0x7ffffu;
    uint32_t v = (immhi << 2) | immlo;
    int64_t sv = (int64_t)(int32_t)(v << 11) >> 11;       /* знак, 21 бит */
    uint64_t page = (va & ~0xfffULL) + (uint64_t)(sv << 12);

    uint32_t imm12 = (w1 >> 10) & 0xfffu;
    *ldr_target = page + (uint64_t)imm12 * 8u;
    return true;
}

/* Уже пропатченная запись: mov w0, #0 / ret / add x16,x16,#imm / br x17.
 * `ldr` затёрт, но `add` и `br` на месте — по ним запись и опознаётся. */
static bool looks_patched(const uint8_t *p) {
    uint32_t w0, w1, w2, w3;
    memcpy(&w0, p + 0, 4);
    memcpy(&w1, p + 4, 4);
    memcpy(&w2, p + 8, 4);
    memcpy(&w3, p + 12, 4);
    if (w0 != PATCH_MOV0 || w1 != PATCH_RET) return false;
    if ((w2 & 0xffc00000u) != 0x91000000u) return false;
    if (((w2 >> 5) & 0x1fu) != 16u || (w2 & 0x1fu) != 16u) return false;
    return w3 == 0xd61f0220u;
}

/* --------------------------------------------- карта трамплинов одного прохода */

typedef struct {
    uint64_t *target;   /* адрес, откуда читает ldr */
    uint64_t *va;       /* сам трамплин */
    size_t    n;
    size_t    cap;
    uint64_t *patched;  /* трамплины, уже несущие патч */
    size_t    npatched;
    size_t    pcap;
} Stubs;

static void stubs_free(Stubs *st) {
    free(st->target);
    free(st->va);
    free(st->patched);
    memset(st, 0, sizeof(*st));
}

static int stubs_add(Stubs *st, uint64_t target, uint64_t va) {
    if (st->n == st->cap) {
        size_t cap = st->cap ? st->cap * 2 : 512;
        uint64_t *t = realloc(st->target, cap * sizeof(uint64_t));
        uint64_t *v = realloc(st->va, cap * sizeof(uint64_t));
        if (!t || !v) { free(t); free(v); return -1; }
        st->target = t;
        st->va = v;
        st->cap = cap;
    }
    st->target[st->n] = target;
    st->va[st->n] = va;
    st->n++;
    return 0;
}

static int stubs_add_patched(Stubs *st, uint64_t va) {
    if (st->npatched == st->pcap) {
        size_t cap = st->pcap ? st->pcap * 2 : 16;
        uint64_t *p = realloc(st->patched, cap * sizeof(uint64_t));
        if (!p) return -1;
        st->patched = p;
        st->pcap = cap;
    }
    st->patched[st->npatched++] = va;
    return 0;
}

static uint64_t stubs_lookup(const Stubs *st, uint64_t target, int *hits) {
    uint64_t va = 0;
    *hits = 0;
    for (size_t i = 0; i < st->n; i++) {
        if (st->target[i] == target) {
            va = st->va[i];
            (*hits)++;
        }
    }
    return va;
}

static bool stubs_is_patched(const Stubs *st, uint64_t va) {
    for (size_t i = 0; i < st->npatched; i++)
        if (st->patched[i] == va) return true;
    return false;
}

/* Один проход по исполняемым PT_LOAD: собрать все канонические трамплины и
 * отметить те, что уже несут патч. */
static int collect_stubs(Src *s, Stubs *st) {
    memset(st, 0, sizeof(*st));
    for (int i = 0; i < s->phnum; i++) {
        const Elf64_Phdr *p = &s->ph[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
        if (p->p_filesz < 16) continue;

        size_t len = (size_t)p->p_filesz;
        uint8_t *buf = malloc(len);
        if (!buf) { stubs_free(st); return -1; }
        if (src_pread(s, p->p_vaddr, buf, len) != 0) {
            warn("%s: не читается исполняемый сегмент 0x%llx", s->label,
                 (unsigned long long)p->p_vaddr);
            free(buf);
            stubs_free(st);
            return -1;
        }

        uint64_t start = (p->p_vaddr + 15u) & ~15ULL;
        for (uint64_t va = start; va + 16 <= p->p_vaddr + p->p_filesz; va += 16) {
            const uint8_t *q = buf + (va - p->p_vaddr);
            uint64_t tgt = 0;
            if (decode_stub(q, va, &tgt)) {
                if (stubs_add(st, tgt, va) != 0) { free(buf); stubs_free(st);
                    return -1; }
                continue;
            }
            if (looks_patched(q)) {
                if (stubs_add_patched(st, va) != 0) { free(buf); stubs_free(st);
                    return -1; }
            }
        }
        free(buf);
    }
    return 0;
}

/* ------------------------------------------------------------ база загрузки */

/* Первая запись /proc/<pid>/maps с offset=0 — это отображение шапки ELF, её
 * начало и есть база загрузки PIE. */
static int find_load_base(pid_t pid, uint64_t *base, char *exe, size_t exelen) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    FILE *f = fopen(path, "re");
    if (!f) return -1;

    char line[4096];
    uint64_t best = 0;
    bool got_exe = false;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long start = 0, end = 0, off = 0;
        char perms[8] = {0};
        if (sscanf(line, "%llx-%llx %7s %llx", &start, &end, perms, &off) != 4)
            continue;
        if (off != 0) continue;
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *sp = strstr(line, " /");
        if (!sp) continue;            /* анонимное отображение не подходит */
        if (best == 0 || start < best) best = start;
        if (exe && !got_exe) {
            snprintf(exe, exelen, "%s", sp + 1);
            got_exe = true;
        }
    }
    fclose(f);
    if (best == 0) return -1;
    *base = best;
    return 0;
}

/* ------------------------------------------------------------- поиск vold */

/* Опознание vold.
 *
 * По comm его опознать НЕЛЬЗЯ: vold зовёт joinThreadPool() из главного потока,
 * и драйвер binder переименовывает этот поток в "binder:<pid>_<n>". На
 * проверенном устройстве /proc/<pid>/comm у vold — "binder:913_2", при том что
 * exe указывает на /system/bin/vold. Поэтому главный признак — exe.
 */
static bool pid_is_vold(pid_t pid, char *exe, size_t exelen) {
    char path[64];
    char buf[4096];

    snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (strcmp(base_name(buf), "vold") == 0) {
            if (exe) snprintf(exe, exelen, "%s", buf);
            return true;
        }
        /* exe читается и это не vold — дальше смотреть нечего. */
        return false;
    }

    /* exe недоступен (нет PTRACE_MODE_READ) — пробуем cmdline. */
    size_t got = 0;
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    if (read_all(path, buf, sizeof(buf) - 1, &got) == 0 && got > 0) {
        buf[got] = '\0';
        for (size_t i = 0; i + 1 < got; i++)
            if (buf[i] == '\0') buf[i] = ' ';
        char *p = buf;
        while (*p == ' ') p++;
        if (strncmp(base_name(p), "vold", 4) == 0) {
            if (exe) snprintf(exe, exelen, "%s", "/system/bin/vold");
            return true;
        }
    }

    /* Последняя попытка — comm (годится, если vold собран без binder-пула на
     * главном потоке). */
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    if (read_all(path, buf, sizeof(buf) - 1, &got) == 0 && got > 0) {
        buf[got] = '\0';
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
        if (strcmp(buf, "vold") == 0) {
            if (exe) snprintf(exe, exelen, "%s", "/system/bin/vold");
            return true;
        }
    }
    return false;
}

static pid_t find_vold(int wait_sec, char *exe, size_t exelen) {
    for (int tick = 0;; tick++) {
        DIR *d = opendir("/proc");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!isdigit((unsigned char)e->d_name[0])) continue;
                pid_t pid = (pid_t)atoi(e->d_name);
                if (pid <= 0) continue;
                if (pid_is_vold(pid, exe, exelen)) {
                    closedir(d);
                    return pid;
                }
            }
            closedir(d);
        }
        if (tick >= wait_sec * 10) return -1;
        usleep(100 * 1000);
    }
}

/* --------------------------------------------------------- чтение/запись памяти */

static int mem_read(pid_t pid, uint64_t addr, void *buf, size_t len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = pread(fd, (char *)buf + off, len - off, (off_t)(addr + off));
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) {
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

static int mem_write(pid_t pid, uint64_t addr, const void *buf, size_t len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = pwrite(fd, buf, len, (off_t)addr);
    int e = errno;
    close(fd);
    if (n != (ssize_t)len) {
        errno = e;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------- разбор */

typedef struct {
    uint64_t got_slot;
    uint64_t stub_va;
    uint32_t sym_index;
    int      reloc_index;
    int      call_sites;
    int      plt_delta;          /* C / 16, посчитан из раскладки .plt */
    bool     already_patched;
} Resolved;

/* Считает `bl`/`b` на трамплин — просто чтобы отчитаться, сколько мест в vold
 * вообще зовут этот символ. */
static int count_call_sites(Src *s, uint64_t stub_va) {
    int hits = 0;
    for (int i = 0; i < s->phnum; i++) {
        const Elf64_Phdr *p = &s->ph[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
        if (p->p_filesz < 4) continue;
        size_t len = (size_t)p->p_filesz;
        uint8_t *buf = malloc(len);
        if (!buf) return hits;
        if (src_pread(s, p->p_vaddr, buf, len) != 0) {
            free(buf);
            continue;
        }
        for (uint64_t off = 0; off + 4 <= len; off += 4) {
            uint32_t w;
            memcpy(&w, buf + off, 4);
            uint32_t op = w & 0xfc000000u;
            if (op != 0x94000000u && op != 0x14000000u) continue;
            int32_t imm = (int32_t)(w & 0x03ffffffu);
            if (imm & 0x02000000) imm |= (int32_t)0xfc000000u;  /* знак, 26 бит */
            uint64_t pc = p->p_vaddr + off;
            if ((uint64_t)((int64_t)pc + (int64_t)imm * 4) == stub_va) hits++;
        }
        free(buf);
    }
    return hits;
}

/* Взят ли адрес символа (любая релокация в .rela.dyn). Если да, патч одного
 * трамплина не покроет такое использование — об этом надо сказать вслух. */
static bool is_address_taken(Src *s, const Dyn *d, uint32_t sym_index) {
    if (!d->reladyn || !d->relasz) return false;
    size_t n = d->relasz / sizeof(Elf64_Rela);
    if (n == 0 || n > 1000000) return false;
    Elf64_Rela *rel = calloc(n, sizeof(Elf64_Rela));
    if (!rel) return false;
    bool found = false;
    if (src_pread(s, d->reladyn, rel, n * sizeof(Elf64_Rela)) == 0) {
        for (size_t i = 0; i < n; i++) {
            if ((uint32_t)(rel[i].r_info >> 32) == sym_index) {
                found = true;
                break;
            }
        }
    }
    free(rel);
    return found;
}

/* Раскладка .plt: для каждой релокации, чей трамплин найден, величина
 * C = va - 16*i обязана быть одной и той же. Возвращает C/16 или -1. */
static int plt_delta(const Stubs *st, const Elf64_Rela *rel, size_t nrel) {
    bool have = false;
    int64_t delta = 0;
    for (size_t i = 0; i < nrel; i++) {
        int hits = 0;
        uint64_t va = stubs_lookup(st, rel[i].r_offset, &hits);
        if (hits != 1) continue;
        int64_t d = ((int64_t)va - (int64_t)i * 16) / 16;
        if (!have) {
            delta = d;
            have = true;
        } else if (d != delta) {
            warn("раскладка .plt нелинейна (релокация %zu даёт %lld, "
                 "ожидалось %lld) — отказываюсь", i, (long long)d,
                 (long long)delta);
            return -1;
        }
    }
    if (!have) {
        warn("не нашлось ни одной пары «релокация -> трамплин»: "
             "раскладку .plt вывести не из чего");
        return -1;
    }
    return (int)delta;
}

static int resolve(Src *s, Resolved *r, bool want_call_sites) {
    memset(r, 0, sizeof(*r));
    r->reloc_index = -1;
    r->plt_delta = -1;

    Dyn d;
    if (read_dynamic(s, &d) != 0) return EXIT_NO_RESOLVE;

    Elf64_Rela *rel = NULL;
    size_t nrel = 0;
    if (load_relocs(s, &d, &rel, &nrel) != 0) return EXIT_NO_RESOLVE;

    /* 1. Релокация символа: её индекс и адрес GOT-слота. */
    for (size_t i = 0; i < nrel; i++) {
        if ((uint32_t)(rel[i].r_info & 0xffffffffu) != R_AARCH64_JUMP_SLOT)
            continue;
        uint32_t idx = (uint32_t)(rel[i].r_info >> 32);
        char nm[512];
        if (sym_name(s, &d, idx, nm, sizeof(nm)) != 0) continue;
        if (strcmp(nm, TARGET_SYM) != 0) continue;
        r->got_slot = rel[i].r_offset;
        r->sym_index = idx;
        r->reloc_index = (int)i;
        break;
    }
    if (r->reloc_index < 0) {
        warn("%s: в .rela.plt нет JUMP_SLOT для \"%s\"", s->label, TARGET_SYM);
        free(rel);
        return EXIT_NO_RESOLVE;
    }

    if (is_address_taken(s, &d, r->sym_index)) {
        warn("%s: адрес \"%s\" ещё и берётся (.rela.dyn) — патч трамплина "
             "покроет только вызовы", s->label, TARGET_SYM);
    }

    /* 2. Все трамплины образа одним проходом. */
    Stubs st;
    if (collect_stubs(s, &st) != 0) {
        free(rel);
        return EXIT_NO_RESOLVE;
    }

    /* 3. Раскладка .plt — по чужим парам, поэтому работает и когда
     *    собственный ldr символа уже затёрт патчем. */
    r->plt_delta = plt_delta(&st, rel, nrel);
    free(rel);
    if (r->plt_delta < 0) {
        stubs_free(&st);
        return EXIT_NO_RESOLVE;
    }

    /* 4. Трамплин символа: сначала прямой поиск по GOT-слоту (свежий vold),
     *    затем — по выведенной раскладке (повторный запуск). */
    int hits = 0;
    uint64_t va = stubs_lookup(&st, r->got_slot, &hits);
    if (hits == 1) {
        r->stub_va = va;
        r->already_patched = false;
    } else if (hits > 1) {
        warn("%s: трамплинов на GOT-слот 0x%llx сразу %d — отказываюсь "
             "угадывать", s->label, (unsigned long long)r->got_slot, hits);
        stubs_free(&st);
        return EXIT_NO_RESOLVE;
    } else {
        int64_t cand = (int64_t)r->plt_delta * 16 +
                       (int64_t)r->reloc_index * 16;
        if (cand <= 0) {
            warn("%s: раскладка дала нелепый адрес трамплина (%lld)",
                 s->label, (long long)cand);
            stubs_free(&st);
            return EXIT_NO_RESOLVE;
        }
        r->stub_va = (uint64_t)cand;
        r->already_patched = stubs_is_patched(&st, r->stub_va);
        if (!r->already_patched) {
            warn("%s: трамплин \"%s\" не найден ни по GOT-слоту 0x%llx, "
                 "ни как пропатченный по 0x%llx", s->label, TARGET_SYM,
                 (unsigned long long)r->got_slot,
                 (unsigned long long)r->stub_va);
            stubs_free(&st);
            return EXIT_NO_RESOLVE;
        }
    }

    if (want_call_sites) r->call_sites = count_call_sites(s, r->stub_va);
    stubs_free(&st);
    return EXIT_OK;
}

/* --------------------------------------------------- самопроверка сопоставления
 *
 * Для КАЖДОЙ релокации JUMP_SLOT требует, чтобы трамплин нашёлся ровно один и
 * чтобы разным символам достались разные трамплины. Если это выполняется,
 * значит «релокация -> трамплин» разрешается однозначно, и никакой адресной
 * арифметики в стиле «rela.plt[i] -> plt + 16*(i+1)» не нужно.
 *
 * На уже пропатченном образе полной биекции быть не может: у пропатченного
 * трамплина нет `ldr`, и декодирование его не находит. Такой пропуск не
 * считается дефектом, но только если их ровно столько, сколько пропатчено, —
 * иначе это была бы настоящая дыра, замаскированная под наш патч.
 */
static int selftest(Src *s) {
    Dyn d;
    if (read_dynamic(s, &d) != 0) return EXIT_NO_RESOLVE;

    Elf64_Rela *rel = NULL;
    size_t n = 0;
    if (load_relocs(s, &d, &rel, &n) != 0) return EXIT_NO_RESOLVE;

    Stubs st;
    if (collect_stubs(s, &st) != 0) {
        free(rel);
        return EXIT_NO_RESOLVE;
    }

    int matched = 0, ambiguous = 0, missing = 0, badtype = 0;
    uint64_t *seen = calloc(n ? n : 1, sizeof(uint64_t));
    size_t nseen = 0;
    for (size_t i = 0; i < n; i++) {
        if ((uint32_t)(rel[i].r_info & 0xffffffffu) != R_AARCH64_JUMP_SLOT) {
            badtype++;
            continue;
        }
        int hits = 0;
        uint64_t va = stubs_lookup(&st, rel[i].r_offset, &hits);
        if (hits == 0) { missing++; continue; }
        if (hits > 1) { ambiguous++; continue; }
        matched++;
        seen[nseen++] = va;
    }

    int dupes = 0;
    for (size_t i = 0; i < nseen; i++)
        for (size_t j = i + 1; j < nseen; j++)
            if (seen[i] == seen[j]) dupes++;
    free(seen);
    free(rel);

    info("%s: JUMP_SLOT %zu, сопоставлено %d, без трамплина %d, "
         "неоднозначных %d, не JUMP_SLOT %d, повторов %d, "
         "трамплинов всего %zu (пропатчено %zu)",
         s->label, n, matched, missing, ambiguous, badtype, dupes,
         st.n, st.npatched);
    size_t npatched = st.npatched;
    stubs_free(&st);

    /* Трамплин, несущий наш патч, по построению теряет `ldr` — декодировать
     * его нельзя, поэтому он и попадает в «без трамплина». Это не дефект
     * раскладки: пропатченных трамплинов ровно столько, сколько записали мы
     * сами. Поэтому пропуск засчитывается, только если он им и объясняется. */
    bool explained = ((size_t)missing == npatched);
    bool ok = (matched + missing == (int)n) && ambiguous == 0 &&
              badtype == 0 && dupes == 0 && explained;
    info("%s: сопоставление %s", s->label,
         ok ? (npatched ? "взаимно однозначное (кроме пропатченных)"
                        : "взаимно однозначное")
            : "НЕПОЛНОЕ");
    return ok ? EXIT_OK : EXIT_NO_RESOLVE;
}

/* --------------------------------------------------------------------- main */

static void usage(void) {
    fputs("usage: vold-noacl [--wait SEC] [--pid PID] [--check] [--dry-run]\n"
          "                  [--file ELF] [--selftest] [--quiet]\n", stderr);
}

int main(int argc, char **argv) {
    int  wait_sec = 0;
    long pid_opt = -1;
    bool check = false, dry_run = false, self = false;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wait") && i + 1 < argc) {
            wait_sec = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--pid") && i + 1 < argc) {
            pid_opt = strtol(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--file") && i + 1 < argc) {
            file = argv[++i];
        } else if (!strcmp(argv[i], "--check")) {
            check = true;
        } else if (!strcmp(argv[i], "--dry-run")) {
            dry_run = true;
        } else if (!strcmp(argv[i], "--selftest")) {
            self = true;
        } else if (!strcmp(argv[i], "--quiet")) {
            g_quiet = true;
        } else {
            usage();
            return EXIT_NO_RESOLVE;
        }
    }

    Src s;
    memset(&s, 0, sizeof(s));
    s.fd = -1;

    char exe[4096] = {0};

    if (file) {
        /* ---- офлайн: разбираем файл, ничего не пишем ---- */
        s.is_proc = false;
        s.label = file;
        s.fd = open(file, O_RDONLY | O_CLOEXEC);
        if (s.fd < 0) {
            warn("%s: %s", file, strerror(errno));
            return EXIT_NO_VOLD;
        }
        if (src_open_header(&s) != 0) {
            src_close(&s);
            return EXIT_NO_RESOLVE;
        }
        int rc = EXIT_OK;
        if (self) {
            rc = selftest(&s);
        } else {
            Resolved r;
            rc = resolve(&s, &r, true);
            if (rc == EXIT_OK) {
                info("%s: %s -> dynsym[%u], .rela.plt[%d], GOT 0x%llx, "
                     "трамплин 0x%llx (сдвиг раскладки %d), вызовов %d%s",
                     s.label, TARGET_SYM, r.sym_index, r.reloc_index,
                     (unsigned long long)r.got_slot,
                     (unsigned long long)r.stub_va, r.plt_delta, r.call_sites,
                     r.already_patched ? ", УЖЕ ПРОПАТЧЕН" : "");

                /* Файл не меняется никогда — писать имеет смысл только в
                 * живой процесс. Но --check и --dry-run обязаны отвечать
                 * одинаково и здесь, и там: иначе офлайн-проверка (в том
                 * числе на хосте, до прошивки) врала бы про «патча нет». */
                uint8_t cur[16];
                if (src_pread(&s, r.stub_va, cur, sizeof(cur)) == 0) {
                    info("%s: сейчас в трамплине %02x %02x %02x %02x "
                         "%02x %02x %02x %02x %02x %02x %02x %02x "
                         "%02x %02x %02x %02x",
                         s.label, cur[0], cur[1], cur[2], cur[3],
                         cur[4], cur[5], cur[6], cur[7],
                         cur[8], cur[9], cur[10], cur[11],
                         cur[12], cur[13], cur[14], cur[15]);

                    bool bytes_patched =
                        memcmp(cur, PATCH_WORDS, sizeof(PATCH_WORDS)) == 0;
                    if (r.already_patched || bytes_patched) {
                        if (!bytes_patched) {
                            warn("%s: трамплин опознан как пропатченный, но "
                                 "байты не совпали (%02x %02x %02x %02x "
                                 "%02x %02x %02x %02x)",
                                 s.label, cur[0], cur[1], cur[2], cur[3],
                                 cur[4], cur[5], cur[6], cur[7]);
                            src_close(&s);
                            return EXIT_NO_RESOLVE;
                        }
                        info("%s: патч уже стоит (mov w0, #0; ret)", s.label);
                    } else {
                        uint64_t tgt = 0;
                        if (!decode_stub(cur, r.stub_va, &tgt) ||
                            tgt != r.got_slot) {
                            warn("%s: в трамплине по 0x%llx не то, что "
                                 "ожидалось — не трогаю",
                                 s.label, (unsigned long long)r.stub_va);
                            src_close(&s);
                            return EXIT_NO_RESOLVE;
                        }
                        if (check) {
                            info("%s: трамплин цел — патча нет (--check)",
                                 s.label);
                            src_close(&s);
                            return 1;
                        }
                        if (!dry_run) {
                            info("%s: файл не меняется — запись только в "
                                 "живой процесс (--dry-run покажет адрес)",
                                 s.label);
                        }
                    }
                }
            }
        }
        src_close(&s);
        return rc;
    }

    /* ---- живой процесс ---- */
    pid_t pid;
    if (pid_opt > 0) {
        pid = (pid_t)pid_opt;
        if (!pid_is_vold(pid, exe, sizeof(exe)))
            warn("pid %ld не похож на vold — продолжаю", pid_opt);
    } else {
        pid = find_vold(wait_sec, exe, sizeof(exe));
        if (pid < 0) {
            warn("vold не найден (ждал %d с)", wait_sec);
            return EXIT_NO_VOLD;
        }
    }

    uint64_t base = 0;
    if (find_load_base(pid, &base, exe, sizeof(exe)) != 0) {
        warn("pid %d: не удалось определить базу загрузки", (int)pid);
        return EXIT_NO_RESOLVE;
    }

    char mempath[64];
    snprintf(mempath, sizeof(mempath), "/proc/%d/mem", (int)pid);

    s.is_proc = true;
    s.base = base;
    s.pid = pid;
    s.label = exe[0] ? exe : "vold";
    s.fd = open(mempath, O_RDONLY | O_CLOEXEC);
    if (s.fd < 0) {
        warn("%s: %s", mempath, strerror(errno));
        return EXIT_NO_RESOLVE;
    }
    if (src_open_header(&s) != 0) {
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }

    Resolved r;
    int rc = resolve(&s, &r, true);
    if (rc != EXIT_OK) {
        src_close(&s);
        return rc;
    }

    uint64_t run_addr = base + r.stub_va;
    info("vold pid=%d база=0x%llx; %s -> .rela.plt[%d], GOT 0x%llx, "
         "трамплин 0x%llx (в процессе 0x%llx), сдвиг раскладки %d, вызовов %d",
         (int)pid, (unsigned long long)base, TARGET_SYM, r.reloc_index,
         (unsigned long long)r.got_slot, (unsigned long long)r.stub_va,
         (unsigned long long)run_addr, r.plt_delta, r.call_sites);

    /* Читаем весь трамплин целиком: сравнивать достаточно первых восьми байт,
     * но разбирать надо все шестнадцать — decode_stub проверяет и `add`, и
     * `br`. */
    uint8_t cur[16];
    if (mem_read(pid, run_addr, cur, sizeof(cur)) != 0) {
        warn("pid %d: не читается 0x%llx", (int)pid,
             (unsigned long long)run_addr);
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }

    bool bytes_patched = memcmp(cur, PATCH_WORDS, sizeof(PATCH_WORDS)) == 0;
    if (r.already_patched || bytes_patched) {
        if (!bytes_patched) {
            warn("трамплин опознан как пропатченный, но байты не совпали "
                 "(%02x %02x %02x %02x %02x %02x %02x %02x) — не трогаю",
                 cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6],
                 cur[7]);
            src_close(&s);
            return EXIT_NO_RESOLVE;
        }
        info("патч уже стоит (mov w0, #0; ret) — ничего не делаю");
        src_close(&s);
        return EXIT_OK;
    }

    /* Перед записью убеждаемся, что в трамплине действительно то, что мы
     * разобрали: adrp + ldr + add + br, и ldr читает именно наш GOT-слот.
     * Иначе адрес вычислен неверно, и писать нельзя. */
    uint64_t tgt = 0;
    if (!decode_stub(cur, r.stub_va, &tgt) || tgt != r.got_slot) {
        warn("в трамплине по 0x%llx не то, что ожидалось "
             "(%02x %02x %02x %02x %02x %02x %02x %02x "
             "%02x %02x %02x %02x %02x %02x %02x %02x) — не трогаю",
             (unsigned long long)run_addr,
             cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6], cur[7],
             cur[8], cur[9], cur[10], cur[11], cur[12], cur[13], cur[14],
             cur[15]);
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }

    if (check) {
        info("трамплин цел — патча нет (--check)");
        src_close(&s);
        return 1;
    }
    if (dry_run) {
        info("--dry-run: записал бы %u байт по 0x%llx",
             (unsigned)sizeof(PATCH_WORDS), (unsigned long long)run_addr);
        src_close(&s);
        return EXIT_OK;
    }

    if (mem_write(pid, run_addr, PATCH_WORDS, sizeof(PATCH_WORDS)) != 0) {
        warn("не удалось записать 0x%llx: %s", (unsigned long long)run_addr,
             strerror(errno));
        src_close(&s);
        return EXIT_NO_WRITE;
    }

    uint8_t back[8];
    if (mem_read(pid, run_addr, back, sizeof(back)) != 0 ||
        memcmp(back, PATCH_WORDS, sizeof(PATCH_WORDS)) != 0) {
        warn("запись не подтвердилась чтением");
        src_close(&s);
        return EXIT_NO_WRITE;
    }

    info("патч поставлен: %s в vold больше не пишет default-ACL "
         "(mov w0, #0; ret по 0x%llx)", TARGET_SYM,
         (unsigned long long)run_addr);

    src_close(&s);
    return EXIT_OK;
}
