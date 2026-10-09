/*
 * func_size.cpp — function sizes from a library's .dynsym on disk.
 *
 * Matched by st_value (= address - base), not by name, so the string table is
 * never read; st_size, not the distance to the next symbol, which overstates
 * the size (locals sit between exports). 0 = unknown, so it is not patched —
 * every failure below is silent and lands in that same "unknown", which
 * tools/hookselftest.cpp is where you see.
 *
 * ~70 KB read once per process at hook install. */

#include "func_size.h"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef STT_GNU_IFUNC
#define STT_GNU_IFUNC 10
#endif

namespace {

struct Target {
    uintptr_t sym_value;  // st_value expected to match the address
    unsigned size;        // st_size
    bool found;           // a symbol with this st_value exists
};

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

// Aliased symbols (same st_value) take the largest size.
void scan_symtab64(const uint8_t *tab, size_t count, size_t entsize,
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

// ELF32: тот же разбор для armeabi-v7a. st_value у Thumb-функции несёт бит
// режима (нечётен), и dlsym отдаёт адрес с тем же битом, поэтому сравнение
// sym_value == st_value сходится и здесь — снимать бит не нужно.
void scan_symtab32(const uint8_t *tab, size_t count, size_t entsize,
                   Target *targets, int n) {
    for (size_t i = 0; i < count; i++) {
        const uint8_t *p = tab + i * entsize;
        const Elf32_Sym *s = reinterpret_cast<const Elf32_Sym *>(p);
        const uint8_t type = ELF32_ST_TYPE(s->st_info);
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

void scan_object64(int fd, const Elf64_Ehdr &eh, Target *targets, int n) {
    const size_t shbytes = static_cast<size_t>(eh.e_shnum) * sizeof(Elf64_Shdr);
    Elf64_Shdr *shdrs = static_cast<Elf64_Shdr *>(malloc(shbytes));
    if (shdrs == nullptr ||
        !read_at(fd, shdrs, shbytes, static_cast<off_t>(eh.e_shoff))) {
        free(shdrs);
        return;
    }

    for (int i = 0; i < eh.e_shnum; i++) {
        const Elf64_Shdr &sh = shdrs[i];
        if (sh.sh_type != SHT_DYNSYM || sh.sh_entsize < sizeof(Elf64_Sym)) continue;
        if (sh.sh_size == 0) continue;

        uint8_t *tab = static_cast<uint8_t *>(malloc(sh.sh_size));
        if (tab == nullptr) continue;
        if (read_at(fd, tab, sh.sh_size, static_cast<off_t>(sh.sh_offset))) {
            scan_symtab64(tab, sh.sh_size / sh.sh_entsize, sh.sh_entsize, targets, n);
        }
        free(tab);
    }

    free(shdrs);
}

void scan_object32(int fd, const Elf32_Ehdr &eh, Target *targets, int n) {
    const size_t shbytes = static_cast<size_t>(eh.e_shnum) * sizeof(Elf32_Shdr);
    Elf32_Shdr *shdrs = static_cast<Elf32_Shdr *>(malloc(shbytes));
    if (shdrs == nullptr ||
        !read_at(fd, shdrs, shbytes, static_cast<off_t>(eh.e_shoff))) {
        free(shdrs);
        return;
    }

    for (int i = 0; i < eh.e_shnum; i++) {
        const Elf32_Shdr &sh = shdrs[i];
        if (sh.sh_type != SHT_DYNSYM || sh.sh_entsize < sizeof(Elf32_Sym)) continue;
        if (sh.sh_size == 0) continue;

        uint8_t *tab = static_cast<uint8_t *>(malloc(sh.sh_size));
        if (tab == nullptr) continue;
        if (read_at(fd, tab, sh.sh_size, static_cast<off_t>(sh.sh_offset))) {
            scan_symtab32(tab, sh.sh_size / sh.sh_entsize, sh.sh_entsize, targets, n);
        }
        free(tab);
    }

    free(shdrs);
}

void scan_object(const char *path, Target *targets, int n) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;

    // Класс и машина — по заголовку, а не по ABI сборки: на arm64-сборке
    // аргументом может оказаться 32-битный объект (и наоборот), и молча
    // вернуть нули из-за неверного класса — худший исход, чем разобрать его.
    uint8_t ident[EI_NIDENT];
    if (!read_at(fd, ident, sizeof ident, 0) || memcmp(ident, ELFMAG, SELFMAG) != 0 ||
        ident[EI_DATA] != ELFDATA2LSB) {
        close(fd);
        return;
    }

    if (ident[EI_CLASS] == ELFCLASS64) {
        Elf64_Ehdr eh;
        if (!read_at(fd, &eh, sizeof eh, 0) || eh.e_machine != EM_AARCH64 ||
            eh.e_shnum == 0 || eh.e_shentsize < sizeof(Elf64_Shdr)) {
            close(fd);
            return;
        }
        scan_object64(fd, eh, targets, n);
    } else if (ident[EI_CLASS] == ELFCLASS32) {
        Elf32_Ehdr eh;
        if (!read_at(fd, &eh, sizeof eh, 0) || eh.e_machine != EM_ARM ||
            eh.e_shnum == 0 || eh.e_shentsize < sizeof(Elf32_Shdr)) {
            close(fd);
            return;
        }
        scan_object32(fd, eh, targets, n);
    }

    close(fd);
}

}  // namespace

void func_sizes(void *const *fns, int n, unsigned *sizes) {
    if (fns == nullptr || sizes == nullptr || n <= 0) return;
    for (int i = 0; i < n; i++) sizes[i] = 0;

    // Object taken from the first function: all hook targets live in libc.
    Dl_info first;
    if (dladdr(fns[0], &first) == 0 || first.dli_fname == nullptr ||
        first.dli_fbase == nullptr || first.dli_fname[0] != '/') {
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(first.dli_fbase);

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
