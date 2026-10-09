/*
 * test-openat-stub.c — проверка src/openat_stub.h на хосте.
 *
 * Что доказывается:
 *
 *   1. На каждом эталонном образе (8 arm64 + 8 arm32) стаб находится РОВНО
 *      один раз.
 *   2. Найденный адрес — это в точности символ __openat из .symtab, а не
 *      «какой-то mov x8,#0x38» (arm64) / «movw r7,#0x142» (arm32). Без этой
 *      сверки «нашли по форме» осталось бы верой: форма без имени не отличает
 *      нужный стаб от похожего.
 *   3. Выведенный из формы размер совпадает с размером символа (arm64: 0x18 на
 *      11–16, 0x14 на 17; arm32: 0x20 везде) — значит размер берётся верно и без
 *      .dynsym.
 *   4. Отказы: пусто, два стаба, mov без svc, короткий буфер — всё -1.
 *
 * Пункт 4 не формальность: именно «нашлось не то» здесь опаснее «не нашлось»,
 * потому что патч уходит по неверному адресу молча.
 *
 * ELF разбирается руками: <elf.h> нет ни в одном тулчейне MSYS2, а хост-тесты
 * этого проекта не должны зависеть от того, чего на хосте нет (см. заголовок
 * tools/proc-name.h). Читаем ровно те поля, которые нужны, и оба класса —
 * ELF64 (AArch64) и ELF32 (ARM): finder'ов теперь два.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openat_stub.h"

static int failures;
static int checks;

static void ok(int cond, const char *what) {
    checks++;
    if (!cond) {
        failures++;
        printf("    ПРОВАЛ: %s\n", what);
    }
}

static unsigned rd16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned rd32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
           ((unsigned)p[3] << 24);
}

static unsigned long long rd64(const unsigned char *p) {
    return (unsigned long long)rd32(p) | ((unsigned long long)rd32(p + 4) << 32);
}

/* --- разбор ELF (оба класса): отдаём исполняемый сегмент и символ __openat -- */

typedef struct {
    const unsigned char *text; /* байты исполняемого сегмента */
    size_t text_len;
    unsigned long long text_addr;
    int is64;                  /* 1 — ELF64/AArch64, 0 — ELF32/ARM */
    int have_sym;
    unsigned long long sym_value;
    unsigned long long sym_size;
} Image;

/* Чтение слова ширины класса. */
static unsigned long long rdw(const unsigned char *p, int is64) {
    return is64 ? rd64(p) : rd32(p);
}

static const unsigned char *shdr(const unsigned char *d, unsigned long long off,
                                 unsigned i, unsigned entsize) {
    return d + off + (unsigned long long)i * entsize;
}

/* Сканируем ИМЕННО исполняемый PT_LOAD, а не .text: в рантайме секций нет,
 * есть только program headers, и модуль получит ровно этот диапазон. Тест,
 * который берёт .text, проверял бы не тот вход. */
static int parse(const unsigned char *d, size_t len, Image *im) {
    if (len < 0x40 || memcmp(d, "\x7f""ELF", 4) != 0) return 0;
    if (d[4] != 1 && d[4] != 2) return 0; /* ELFCLASS32/64 */
    if (d[5] != 1) return 0;              /* ELFDATA2LSB */
    im->is64 = (d[4] == 2);

    unsigned long long phoff, shoff;
    unsigned phentsize, phnum, shentsize, shnum, shstrndx;
    if (im->is64) {
        phoff = rd64(d + 0x20);
        phentsize = rd16(d + 0x36);
        phnum = rd16(d + 0x38);
        shoff = rd64(d + 0x28);
        shentsize = rd16(d + 0x3a);
        shnum = rd16(d + 0x3c);
        shstrndx = rd16(d + 0x3e);
    } else {
        phoff = rd32(d + 0x1c);
        phentsize = rd16(d + 0x2a);
        phnum = rd16(d + 0x2c);
        shoff = rd32(d + 0x20);
        shentsize = rd16(d + 0x2e);
        shnum = rd16(d + 0x30);
        shstrndx = rd16(d + 0x32);
    }
    if (phoff == 0 || phnum == 0) return 0;
    if (phoff + (unsigned long long)phnum * phentsize > len) return 0;

    for (unsigned i = 0; i < phnum; i++) {
        const unsigned char *p = d + phoff + (unsigned long long)i * phentsize;
        if (rd32(p + 0x00) != 1u) continue; /* PT_LOAD */
        unsigned long long off, filesz, memsz;
        unsigned flags;
        if (im->is64) {
            flags = rd32(p + 0x04);
            off = rd64(p + 0x08);
            im->text_addr = rd64(p + 0x10);
            filesz = rd64(p + 0x20);
            memsz = rd64(p + 0x28);
        } else {
            off = rd32(p + 0x04);
            im->text_addr = rd32(p + 0x08);
            filesz = rd32(p + 0x10);
            memsz = rd32(p + 0x14);
            flags = rd32(p + 0x18);
        }
        if ((flags & 1u) == 0) continue; /* без PF_X — не он */
        if (off + filesz > len) return 0;
        if (filesz != memsz) return 0; /* исполняемый сегмент не бывает с bss */
        if ((off & 3u) != 0) return 0; /* на uint32* такое бросать нельзя */
        im->text = d + off;
        im->text_len = (size_t)filesz;
        break;
    }
    if (im->text == NULL) return 0;

    if (shoff == 0 || shnum == 0 || shstrndx >= shnum) return 0;
    if (shoff + (unsigned long long)shnum * shentsize > len) return 0;

    /* Смещение/размер у shdr: ELF64 0x18/0x20, ELF32 0x10/0x14. */
    const unsigned sh_off_f = im->is64 ? 0x18u : 0x10u;
    const unsigned sh_size_f = im->is64 ? 0x20u : 0x14u;

    int sym_sec = -1;
    for (unsigned i = 0; i < shnum; i++) {
        const unsigned char *s = shdr(d, shoff, i, shentsize);
        const unsigned n = rd32(s + 0);
        const unsigned char *shstr = shdr(d, shoff, shstrndx, shentsize);
        const unsigned char *names = d + rdw(shstr + sh_off_f, im->is64);
        const unsigned long long names_len = rdw(shstr + sh_size_f, im->is64);
        if (n >= names_len) continue;
        if (strcmp((const char *)names + n, ".symtab") == 0) sym_sec = (int)i;
    }
    if (sym_sec < 0) return 0;

    {
        const unsigned char *s = shdr(d, shoff, (unsigned)sym_sec, shentsize);
        const unsigned long long off = rdw(s + sh_off_f, im->is64);
        const unsigned long long sz = rdw(s + sh_size_f, im->is64);
        const unsigned entsize = (unsigned)rdw(s + (im->is64 ? 0x38u : 0x24u), im->is64);
        const unsigned link = rd32(s + (im->is64 ? 0x28u : 0x18u));
        if (link >= shnum) return 0;
        const unsigned char *ls = shdr(d, shoff, link, shentsize);
        const unsigned char *strs = d + rdw(ls + sh_off_f, im->is64);
        const unsigned long long strs_len = rdw(ls + sh_size_f, im->is64);
        if (entsize == 0) return 0;
        /* st_value: ELF64 0x08, ELF32 0x04; st_size: ELF64 0x10, ELF32 0x08. */
        const unsigned st_value_f = im->is64 ? 0x08u : 0x04u;
        const unsigned st_size_f = im->is64 ? 0x10u : 0x08u;
        for (unsigned long long o = 0; o + entsize <= sz; o += entsize) {
            const unsigned char *e = d + off + o;
            const unsigned n = rd32(e + 0);
            if (n >= strs_len) continue;
            if (strcmp((const char *)strs + n, "__openat") != 0) continue;
            im->have_sym = 1;
            im->sym_value = rdw(e + st_value_f, im->is64);
            im->sym_size = rdw(e + st_size_f, im->is64);
            break;
        }
    }
    return 1;
}

/* --- сам тест ---------------------------------------------------------- */

static void check_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        failures++;
        printf("  %s: не открыть\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = (unsigned char *)malloc((size_t)sz);
    if (d == NULL || fread(d, 1, (size_t)sz, f) != (size_t)sz) {
        failures++;
        printf("  %s: не прочитать (%ld байт)\n", path, sz);
        fclose(f);
        free(d);
        return;
    }
    fclose(f);

    Image im;
    memset(&im, 0, sizeof im);
    if (!parse(d, (size_t)sz, &im)) {
        failures++;
        printf("  %s: не разобрать как ELF с исполняемым сегментом и .symtab\n", path);
        free(d);
        return;
    }

    const int is64 = im.is64;
    unsigned size = 0;
    const long off = is64 ? openat_stub_find_a64(im.text, im.text_len, &size)
                          : openat_stub_find_arm(im.text, im.text_len, &size);
    const unsigned min = is64 ? OPENAT_STUB_MIN_SIZE : OPENAT_STUB_ARM_MIN_SIZE;

    printf("  %-24s %-5s смещение=%ld размер=%u", path, is64 ? "ELF64" : "ELF32", off, size);
    if (im.have_sym)
        printf("  символ __openat=0x%llx/%llu", im.sym_value, im.sym_size);
    printf("\n");

    ok(off >= 0, "стаб должен найтись ровно один раз");
    ok(im.have_sym, "в .symtab должен быть символ __openat");
    if (off >= 0 && im.have_sym) {
        ok((unsigned long long)off + im.text_addr == im.sym_value,
           "найденное смещение + адрес .text должно равняться адресу символа");
        ok((unsigned long long)size == im.sym_size,
           "выведенный из формы размер должен равняться размеру символа");
    }
    ok(size >= min || off < 0, "размер стаба не должен быть меньше ширины патча");

    /* Контроль уникальности: опорная форма встречается ровно один раз во всём
     * исполняемом сегменте. Это независимое от finder'а утверждение — если оно
     * сломается, finder может остаться однозначным случайно.
     *
     * На arm64 опорное слово — mov x8,#0x38, и оно уникально само по себе.
     * На arm32 `mov r12,r7` уникальным НЕ является (это обычная инструкция
     * сохранения r7), поэтому опорой служит вся тройка пролога: mov r12,r7;
     * movw r7,#0x142; svc #0. */
    {
        unsigned n = 0;
        const uint32_t *w = (const uint32_t *)im.text;
        const size_t nw = im.text_len / 4;
        if (is64) {
            for (size_t i = 0; i < nw; i++)
                if (w[i] == OPENAT_STUB_MOV_X8_38) n++;
        } else {
            for (size_t i = 0; i + 2 < nw; i++)
                if (w[i] == OPENAT_STUB_ARM_MOV_R12_R7 &&
                    w[i + 1] == OPENAT_STUB_ARM_MOVW_R7_142 &&
                    w[i + 2] == OPENAT_STUB_ARM_SVC_0) n++;
        }
        ok(n == 1, "опорная форма должна встречаться ровно один раз");
    }
    free(d);
}

static void check_refusals(void) {
    unsigned size = 0;

    /* ---- AArch64 (синтетика) ---- */
    {
        static const uint32_t one[6] = {0xd2800708u, 0xd4000001u, 0xb140041fu, 0xda809400u,
                                        0x54ff56e8u, 0xd65f03c0u};
        static uint32_t two[14];
        static const uint32_t no_svc[6] = {0xd2800708u, 0xd503201fu, 0xb140041fu, 0xda809400u,
                                           0x54ff56e8u, 0xd65f03c0u};
        static const uint32_t no_ret[8] = {0xd2800708u, 0xd4000001u, 0xd503201fu, 0xd503201fu,
                                           0xd503201fu, 0xd503201fu, 0xd503201fu, 0xd503201fu};

        ok(openat_stub_find_a64(one, sizeof one, &size) == 0 && size == 24,
           "a64: один стаб — смещение 0, размер 24");
        memcpy(two, one, sizeof one);
        memcpy(two + 8, one, sizeof one); /* второй стаб на смещении 32 */
        ok(openat_stub_find_a64(two, sizeof two, &size) == -1, "a64: два стаба → -1");
        ok(openat_stub_find_a64(no_svc, sizeof no_svc, &size) == -1, "a64: mov без svc → -1");
        ok(openat_stub_find_a64(no_ret, sizeof no_ret, &size) == -1, "a64: svc без ret рядом → -1");
        ok(openat_stub_find_a64(one, 8, &size) == -1, "a64: буфер короче минимума → -1");
        ok(openat_stub_find_a64(NULL, 64, &size) == -1, "a64: NULL → -1");

        /* Новая форма 17: ret на +0x10, размер 20 — ровно ширина патча. */
        static const uint32_t a17[5] = {0xd2800708u, 0xd4000001u, 0xb13ffc1fu,
                                        0x54cdf2c2u, 0xd65f03c0u};
        ok(openat_stub_find_a64(a17, sizeof a17, &size) == 0 && size == 20,
           "a64: новая форма 17 — смещение 0, размер 20");
    }

    /* ---- ARM32 (синтетика) ---- */
    {
        /* mov r12,r7; movw r7,#0x142; svc #0; mov r7,r12; cmn r0,#4096; bxls lr;
         * rsb r0,r0,#0; b <куда угодно> — 32 байта, как в эталонах. */
        static const uint32_t one[8] = {0xe1a0c007u, 0xe3007142u, 0xef000000u, 0xe1a0700cu,
                                        0xe3700a01u, 0x912fff1eu, 0xe2600000u, 0xea003e17u};
        static uint32_t two[16];
        static const uint32_t no_svc[8] = {0xe1a0c007u, 0xe3007142u, 0xe320f000u, 0xe1a0700cu,
                                           0xe3700a01u, 0x912fff1eu, 0xe2600000u, 0xea003e17u};
        /* Пролог есть, movw есть, svc есть, но хвостового B нет вовсе. */
        static const uint32_t no_b[8] = {0xe1a0c007u, 0xe3007142u, 0xef000000u, 0xe1a0700cu,
                                         0xe3700a01u, 0x912fff1eu, 0xe2600000u, 0xe1a00000u};
        /* movw r7,#0x142 без пролога: форма обёртки не опознаётся. */
        static const uint32_t no_prologue[8] = {0xe320f000u, 0xe3007142u, 0xef000000u,
                                                0xe1a0700cu, 0xe3700a01u, 0x912fff1eu,
                                                0xe2600000u, 0xea003e17u};

        ok(openat_stub_find_arm(one, sizeof one, &size) == 0 && size == 32,
           "arm: один стаб — смещение 0, размер 32");
        memcpy(two, one, sizeof one);
        memcpy(two + 8, one, sizeof one); /* второй стаб на смещении 32 */
        ok(openat_stub_find_arm(two, sizeof two, &size) == -1, "arm: два стаба → -1");
        ok(openat_stub_find_arm(no_svc, sizeof no_svc, &size) == -1, "arm: mov без svc → -1");
        ok(openat_stub_find_arm(no_b, sizeof no_b, &size) == -1, "arm: без хвостового B → -1");
        ok(openat_stub_find_arm(no_prologue, sizeof no_prologue, &size) == -1,
           "arm: без пролога mov r12,r7 → -1");
        ok(openat_stub_find_arm(one, 4, &size) == -1, "arm: буфер короче минимума → -1");
        ok(openat_stub_find_arm(NULL, 64, &size) == -1, "arm: NULL → -1");
    }
}

int main(int argc, char **argv) {
    printf("отказы (синтетика):\n");
    check_refusals();

    if (argc > 1) {
        printf("образы:\n");
        for (int i = 1; i < argc; i++) check_image(argv[i]);
    }

    printf("\nпроверок=%d провалов=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
