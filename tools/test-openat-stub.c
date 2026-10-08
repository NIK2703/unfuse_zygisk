/*
 * test-openat-stub.c — проверка src/openat_stub.h на хосте.
 *
 * Что доказывается:
 *
 *   1. На каждом эталонном образе стаб находится РОВНО один раз.
 *   2. Найденный адрес — это в точности символ __openat из .symtab, а не
 *      «какой-то mov x8,#0x38». Без этой сверки «нашли по форме» осталось бы
 *      верой: форма без имени не отличает нужный стаб от похожего.
 *   3. Выведенный из формы размер совпадает с размером символа (0x18 на 11–16,
 *      0x14 на 17) — значит размер берётся верно и без .dynsym.
 *   4. Отказы: пусто, два стаба, mov без svc, короткий буфер — всё -1.
 *
 * Пункт 4 не формальность: именно «нашлось не то» здесь опаснее «не нашлось»,
 * потому что патч уходит по неверному адресу молча.
 *
 * ELF разбирается руками: <elf.h> нет ни в одном тулчейне MSYS2, а хост-тесты
 * этого проекта не должны зависеть от того, чего на хосте нет (см. заголовок
 * tools/proc-name.h). Читаем ровно те поля, которые нужны.
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

/* --- разбор ELF64: отдаём исполняемый сегмент и символ __openat --------- */

typedef struct {
    const unsigned char *text; /* байты исполняемого сегмента */
    size_t text_len;
    unsigned long long text_addr;
    int have_sym;
    unsigned long long sym_value;
    unsigned long long sym_size;
} Image;

static const unsigned char *shdr(const unsigned char *d, unsigned long long off,
                                 unsigned i, unsigned entsize) {
    return d + off + (unsigned long long)i * entsize;
}

/* Сканируем ИМЕННО исполняемый PT_LOAD, а не .text: в рантайме секций нет,
 * есть только program headers, и модуль получит ровно этот диапазон. Тест,
 * который берёт .text, проверял бы не тот вход. */
static int parse(const unsigned char *d, size_t len, Image *im) {
    if (len < 0x40 || memcmp(d, "\x7f""ELF", 4) != 0) return 0;
    if (d[4] != 2) return 0; /* ELF64 */

    const unsigned long long phoff = rd64(d + 0x20);
    const unsigned phentsize = rd16(d + 0x36);
    const unsigned phnum = rd16(d + 0x38);
    if (phoff == 0 || phnum == 0) return 0;
    if (phoff + (unsigned long long)phnum * phentsize > len) return 0;

    for (unsigned i = 0; i < phnum; i++) {
        const unsigned char *p = d + phoff + (unsigned long long)i * phentsize;
        if (rd32(p + 0x00) != 1u) continue;      /* PT_LOAD */
        if ((rd32(p + 0x04) & 1u) == 0) continue; /* без PF_X — не он */
        const unsigned long long off = rd64(p + 0x08);
        const unsigned long long filesz = rd64(p + 0x20);
        const unsigned long long memsz = rd64(p + 0x28);
        if (off + filesz > len) return 0;
        if (filesz != memsz) return 0; /* исполняемый сегмент не бывает с bss */
        if ((off & 3u) != 0) return 0; /* на uint32* такое бросать нельзя */
        im->text = d + off;
        im->text_len = (size_t)filesz;
        im->text_addr = rd64(p + 0x10);
        break;
    }
    if (im->text == NULL) return 0;

    const unsigned long long shoff = rd64(d + 0x28);
    const unsigned shentsize = rd16(d + 0x3a);
    const unsigned shnum = rd16(d + 0x3c);
    const unsigned shstrndx = rd16(d + 0x3e);
    if (shoff == 0 || shnum == 0 || shstrndx >= shnum) return 0;
    if (shoff + (unsigned long long)shnum * shentsize > len) return 0;

    int sym_sec = -1;
    for (unsigned i = 0; i < shnum; i++) {
        const unsigned char *s = shdr(d, shoff, i, shentsize);
        const unsigned n = rd32(s + 0);
        const unsigned char *names =
            d + rd64(shdr(d, shoff, shstrndx, shentsize) + 0x18);
        const unsigned long long names_len =
            rd64(shdr(d, shoff, shstrndx, shentsize) + 0x20);
        if (n >= names_len) continue;
        if (strcmp((const char *)names + n, ".symtab") == 0) sym_sec = (int)i;
    }
    if (sym_sec < 0) return 0;

    {
        const unsigned char *s = shdr(d, shoff, (unsigned)sym_sec, shentsize);
        const unsigned long long off = rd64(s + 0x18);
        const unsigned long long sz = rd64(s + 0x20);
        const unsigned entsize = (unsigned)rd64(s + 0x38);
        const unsigned link = rd32(s + 0x28);
        if (link >= shnum) return 0;
        const unsigned char *strs =
            d + rd64(shdr(d, shoff, link, shentsize) + 0x18);
        const unsigned long long strs_len =
            rd64(shdr(d, shoff, link, shentsize) + 0x20);
        if (entsize == 0) return 0;
        for (unsigned long long o = 0; o + entsize <= sz; o += entsize) {
            const unsigned char *e = d + off + o;
            const unsigned n = rd32(e + 0);
            if (n >= strs_len) continue;
            if (strcmp((const char *)strs + n, "__openat") != 0) continue;
            im->have_sym = 1;
            im->sym_value = rd64(e + 0x08);
            im->sym_size = rd64(e + 0x10);
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
        printf("  %s: не разобрать как ELF64 с исполняемым сегментом и .symtab\n", path);
        free(d);
        return;
    }

    unsigned size = 0;
    const long off = openat_stub_find(im.text, im.text_len, &size);

    printf("  %-24s смещение=%ld размер=%u", path, off, size);
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
    ok(size >= OPENAT_STUB_MIN_SIZE || off < 0,
       "размер стаба не должен быть меньше ширины патча");

    /* Контроль уникальности: mov x8,#0x38 встречается ровно один раз во всём
     * исполняемом сегменте. Это независимое от finder'а утверждение — если оно
     * сломается, finder может остаться однозначным случайно. */
    {
        unsigned movs = 0;
        for (size_t i = 0; i < im.text_len / 4; i++)
            if (((const uint32_t *)im.text)[i] == OPENAT_STUB_MOV_X8_38) movs++;
        ok(movs == 1, "mov x8,#0x38 должен встречаться в сегменте ровно один раз");
    }
    free(d);
}

static void check_refusals(void) {
    static const uint32_t one[6] = {0xd2800708u, 0xd4000001u, 0xb140041fu, 0xda809400u,
                                    0x54ff56e8u, 0xd65f03c0u};
    static uint32_t two[14];
    static const uint32_t no_svc[6] = {0xd2800708u, 0xd503201fu, 0xb140041fu, 0xda809400u,
                                       0x54ff56e8u, 0xd65f03c0u};
    static const uint32_t no_ret[8] = {0xd2800708u, 0xd4000001u, 0xd503201fu, 0xd503201fu,
                                       0xd503201fu, 0xd503201fu, 0xd503201fu, 0xd503201fu};
    unsigned size = 0;

    /* Заведомо годный: ровно один стаб. */
    ok(openat_stub_find(one, sizeof one, &size) == 0 && size == 24,
       "один стаб: смещение 0, размер 24");

    /* Два стаба — отказ, а не выбор первого. */
    memcpy(two, one, sizeof one);
    memcpy(two + 8, one, sizeof one); /* второй стаб на смещении 32 */
    ok(openat_stub_find(two, sizeof two, &size) == -1, "два стаба → -1");

    ok(openat_stub_find(no_svc, sizeof no_svc, &size) == -1, "mov без svc → -1");
    ok(openat_stub_find(no_ret, sizeof no_ret, &size) == -1, "svc без ret рядом → -1");
    ok(openat_stub_find(one, 8, &size) == -1, "буфер короче минимума → -1");
    ok(openat_stub_find(NULL, 64, &size) == -1, "NULL → -1");

    /* Новая форма 17: ret на +0x10, размер 20 — ровно ширина патча. */
    {
        static const uint32_t a17[5] = {0xd2800708u, 0xd4000001u, 0xb13ffc1fu,
                                        0x54cdf2c2u, 0xd65f03c0u};
        ok(openat_stub_find(a17, sizeof a17, &size) == 0 && size == 20,
           "новая форма 17: смещение 0, размер 20");
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
