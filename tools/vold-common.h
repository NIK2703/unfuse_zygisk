/*
 * vold-common.h — the ELF-reading half that both vold patchers share.
 *
 * vold-noacl.c and vold-fusefs.c answer the same question about a running vold:
 * "where is the .plt trampoline of this imported libc symbol, and what does it
 * currently contain". Both identify vold by its exe (not comm), read its ELF
 * header and program headers through /proc/<pid>/mem — or from the file itself
 * for --file — walk PT_DYNAMIC for .rela.plt, and resolve a name through
 * .dynsym/.dynstr. That half was copied between them, and by 2026-10-08 the two
 * copies had already drifted; it lives here so there is one copy to keep right.
 *
 * Only that half is here. Each tool's own half stays in its own file, because
 * it is genuinely different: vold-noacl overwrites one trampoline in place with
 * `mov w0,#0 ; ret`, vold-fusefs redirects two of them to a generated arm64
 * handler. Nor is anything shared with the Zygisk module (src/): it patches a
 * different process, through a different mechanism (in-memory function entries,
 * not ELF tables).
 *
 * Everything here is `static`: this is a header included by two single-file
 * programs, not a library, and nothing in it should acquire a symbol in either
 * binary.
 *
 * ============================== what had drifted
 *
 * The differences were all one-directional — the vold-noacl copy was the richer
 * one — so both are kept here as the union and neither tool loses anything:
 *
 *   - src_open_header: it names e_ident[EI_CLASS] in the "not ELF64" warning
 *     ("class=%u"), which is what tells a 32-bit image apart from a corrupt
 *     one; the vold-fusefs copy had dropped the argument.
 *   - Dyn / read_dynamic: it reads DT_RELA and DT_RELASZ into reladyn/relasz,
 *     which vold-noacl's is_address_taken() needs to spot a symbol whose
 *     address was taken; the vold-fusefs copy had no such fields. The two extra
 *     cases cost vold-fusefs nothing: it simply never looks at the fields.
 *
 * The other three that differed — src_pread, decode_stub, find_load_base —
 * differed only in comments, and the fuller comments are the ones kept.
 *
 * ============================== what is deliberately NOT here
 *
 * info() and warn() are not: each tool prefixes its own name ("vold-noacl: " /
 * "vold-fusefs: "), which is the entire point of them. The code below calls
 * both, so the two prototypes are the contract an including .c has to satisfy.
 *
 * Neither is the stub collection. vold-noacl wants a list of every trampoline
 * plus the ones already patched (Stubs, collect_stubs, stubs_*), while
 * vold-fusefs wants a single named stub and its original bytes (hook_resolve,
 * hook_stub_intact, hook_install). Those are two different jobs that happen to
 * walk the same segment, not one job written twice.
 */

#pragma once

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
#include <sys/types.h>
#include <unistd.h>

/* Defined by each tool, with its own name as the prefix. */
static void info(const char *fmt, ...);
static void warn(const char *fmt, ...);

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

/* Same parse for a file (--file) and a live process; only VA->byte differs: a file
 * uses PT_LOAD, a process reads base+VA from /proc/<pid>/mem. */

typedef struct {
    bool      is_proc;
    int       fd;
    uint64_t  base;
    pid_t     pid;
    const char *label;

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

    /* File: VA -> offset via PT_LOAD; else VA is already the offset (header, phdrs). */
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

typedef struct {
    uint64_t symtab, strtab, strsz;
    uint64_t jmprel, pltrelsz, pltrel, relaent;
    uint64_t reladyn, relasz;   /* needed to detect "address taken" */
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

/* Canonical AArch64 stub: adrp x16,<page> / ldr x17,[x16,#imm12*8] /
 * add x16,x16,#imm12*8 / br x17. Returns the address `ldr` reads from; the strict
 * form check (all four insns, x16/x17) avoids false scan matches. */
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
    int64_t sv = (int64_t)(int32_t)(v << 11) >> 11;       /* sign-extend 21 bits */
    uint64_t page = (va & ~0xfffULL) + (uint64_t)(sv << 12);

    uint32_t imm12 = (w1 >> 10) & 0xfffu;
    *ldr_target = page + (uint64_t)imm12 * 8u;
    return true;
}

/* First /proc/<pid>/maps entry with offset=0 is the ELF header mapping; its start
 * is the PIE load base. Match the wanted file (want_exe), not the lowest address
 * overall (linker/libc may lie below); lowest is only a fallback when want_exe is
 * unknown. Returns 0 found, -1 maps unreadable, -2 no usable entry. */
static int find_load_base(pid_t pid, const char *want_exe, uint64_t *base,
                          char *exe_out, size_t exelen) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    FILE *f = fopen(path, "re");
    if (!f) return -1;

    char line[4096];
    uint64_t exact = 0, best = 0;
    char best_path[4096] = {0};

    while (fgets(line, sizeof(line), f)) {
        unsigned long long start = 0, end = 0, off = 0;
        char perms[8] = {0};
        if (sscanf(line, "%llx-%llx %7s %llx", &start, &end, perms, &off) != 4)
            continue;
        if (off != 0) continue;
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *sp = strstr(line, " /");
        if (!sp) continue;
        char *mp = sp + 1;
        /* Kernel appends " (deleted)" to a replaced file's path — strip it before comparing. */
        char *del = strstr(mp, " (deleted)");
        if (del) *del = '\0';

        if (want_exe && want_exe[0] && strcmp(mp, want_exe) == 0) {
            if (exact == 0 || start < exact) exact = start;
        }
        if (best == 0 || start < best) {
            best = start;
            snprintf(best_path, sizeof(best_path), "%s", mp);
        }
    }
    fclose(f);

    if (exact != 0) {
        if (exe_out) snprintf(exe_out, exelen, "%s", want_exe);
        *base = exact;
        return 0;
    }
    if (best != 0) {
        if (exe_out) snprintf(exe_out, exelen, "%s", best_path);
        *base = best;
        return 0;
    }
    return -2;
}

/* Identify vold by exe, not comm: vold calls joinThreadPool() from the main thread
 * and binder renames it to "binder:<pid>_<n>". */
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
        return false;
    }

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
