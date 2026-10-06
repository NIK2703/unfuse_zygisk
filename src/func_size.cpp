/*
 * func_size.cpp — размеры функций из .dynsym библиотеки на диске.
 *
 * Как это работает. По адресу функции dladdr() отдаёт путь к объекту и его
 * базовый адрес. Дальше файл читается через pread: заголовок, таблица секций,
 * таблица символов. Символ ищется НЕ по имени, а по st_value: мы уже знаем
 * рантайм-адрес, значит знаем и st_value = адрес - база, а сравнение
 * шестидесятичетырёхбитных чисел избавляет от чтения таблицы строк целиком.
 *
 * Почему st_size, а не «расстояние до следующего символа». Расстояние до
 * следующего ДИНАМИЧЕСКОГО символа — не размер: между двумя экспортированными
 * функциями в .text сплошь и рядом лежат локальные, и расстояние окажется больше
 * настоящего размера, то есть опаснее, чем отсутствие проверки. Поэтому если
 * st_size == 0, размер считается неизвестным, и функция не патчится. Такой
 * проход только в сторону «не тронуть лишнего».
 *
 * Читается около 70 КБ на процесс (заголовок, таблица секций и .dynsym), один
 * раз при установке хуков. Таблица строк не читается вовсе.
 */

#include "func_size.h"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <android/log.h>

#define LOG_TAG "SdcardFsRestore"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

#ifndef STT_GNU_IFUNC
#define STT_GNU_IFUNC 10
#endif

namespace {

struct Target {
    uintptr_t sym_value;  // st_value, который должен соответствовать адресу
    unsigned size;        // st_size
    bool found;           // символ с таким st_value в таблице есть
};

// Читает ровно count байт по смещению off. Возвращает false при коротком чтении.
bool read_at(int fd, void *buf, size_t count, off_t off) {
    uint8_t *p = static_cast<uint8_t *>(buf);
    size_t done = 0;
    while (done < count) {
        const ssize_t n = pread(fd, p + done, count - done, off + static_cast<off_t>(done));
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

// Один проход по таблице символов: для каждой цели ищет символ с её st_value.
// Если символов с одинаковым st_value несколько (так бывает, когда один адрес
// носят несколько имён), берётся наибольший размер — он безопаснее.
void scan_symtab(const uint8_t *tab, size_t count, size_t entsize,
                 Target *targets, int n) {
    for (size_t i = 0; i < count; i++) {
        const uint8_t *p = tab + i * entsize;
        const Elf64_Sym *s = reinterpret_cast<const Elf64_Sym *>(p);
        const uint8_t type = ELF64_ST_TYPE(s->st_info);
        if (type != STT_FUNC && type != STT_GNU_IFUNC) continue;
        if (s->st_value == 0) continue;

        for (int k = 0; k < n; k++) {
            if (s->st_value != targets[k].sym_value) continue;
            const unsigned sz = static_cast<unsigned>(s->st_size);
            if (!targets[k].found || sz > targets[k].size) targets[k].size = sz;
            targets[k].found = true;
        }
    }
}

// Разбирает один объект: заголовок, секции, .dynsym.
void scan_object(const char *path, Target *targets, int n) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGW("размеры: не открыть %s", path);
        return;
    }

    Elf64_Ehdr eh;
    if (!read_at(fd, &eh, sizeof eh, 0) ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB ||
        eh.e_machine != EM_AARCH64 ||
        eh.e_shnum == 0 || eh.e_shentsize < sizeof(Elf64_Shdr)) {
        close(fd);
        LOGW("размеры: %s не ELF64/AArch64", path);
        return;
    }

    const size_t shbytes = static_cast<size_t>(eh.e_shnum) * sizeof(Elf64_Shdr);
    Elf64_Shdr *shdrs = static_cast<Elf64_Shdr *>(malloc(shbytes));
    if (shdrs == nullptr ||
        !read_at(fd, shdrs, shbytes, static_cast<off_t>(eh.e_shoff))) {
        free(shdrs);
        close(fd);
        return;
    }

    for (int i = 0; i < eh.e_shnum; i++) {
        const Elf64_Shdr &sh = shdrs[i];
        if (sh.sh_type != SHT_DYNSYM || sh.sh_entsize < sizeof(Elf64_Sym)) continue;
        if (sh.sh_size == 0) continue;

        uint8_t *tab = static_cast<uint8_t *>(malloc(sh.sh_size));
        if (tab == nullptr) continue;
        if (read_at(fd, tab, sh.sh_size, static_cast<off_t>(sh.sh_offset))) {
            scan_symtab(tab, sh.sh_size / sh.sh_entsize, sh.sh_entsize, targets, n);
        }
        free(tab);
    }

    free(shdrs);
    close(fd);
}

}  // namespace

void func_sizes(void *const *fns, int n, unsigned *sizes) {
    if (fns == nullptr || sizes == nullptr || n <= 0) return;
    for (int i = 0; i < n; i++) sizes[i] = 0;

    // Объект берём по первой функции: все цели хука лежат в libc.
    Dl_info first;
    if (dladdr(fns[0], &first) == 0 || first.dli_fname == nullptr ||
        first.dli_fbase == nullptr || first.dli_fname[0] != '/') {
        LOGW("размеры: не определить объект для %p", fns[0]);
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(first.dli_fbase);

    // Цели, лежащие в этом же объекте: st_value = адрес - база.
    Target targets[32];
    int map[32];
    int m = 0;
    for (int i = 0; i < n && m < 32; i++) {
        Dl_info di;
        if (dladdr(fns[i], &di) == 0 || di.dli_fbase != first.dli_fbase) continue;
        const uintptr_t addr = reinterpret_cast<uintptr_t>(fns[i]);
        if (addr < base) continue;
        targets[m].sym_value = addr - base;
        targets[m].size = 0;
        targets[m].found = false;
        map[m] = i;
        m++;
    }
    if (m == 0) return;

    scan_object(first.dli_fname, targets, m);

    for (int k = 0; k < m; k++) {
        if (targets[k].found) sizes[map[k]] = targets[k].size;
    }
}
