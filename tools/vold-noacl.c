/*
 * vold-noacl — make vold::SetDefaultAcl() a no-op in a running vold, without
 *              depending on how that vold was built.
 *
 * ============================== why
 *
 * Preparing a user's CE storage, vold calls SetDefaultAcl(...) (FsCrypt.cpp:1027 on
 * 16) and sets a default ACL on /data/media/<user> naming group 1023 (media_rw). The
 * module grants shared-storage access via a named entry for 9997 (AID_EVERYBODY);
 * new dirs inherit whatever wrote the default ACL last, and vold writes later (CE
 * prep, after post-fs-data), so dirs created by anything but the app inherit 1023
 * and apps cannot see them. The other two call sites (Utils.cpp:406, :1889 on 16)
 * hurt too — and on 11 those are the ONLY ones: there is no FsCrypt.cpp caller at
 * all there, because its CE prep goes through the local prepare_dir() ->
 * fs_prepare_dir(), which sets mode and owner and no ACL. Fewer call sites, the same
 * damage, and the patch covers it identically — see "why the ELF tables" below.
 *
 * AOSP already has a no-op branch (vold-16/Utils.cpp:142): `if (IsSdcardfsUsed())
 * return OK;`, gated by the property external_storage.sdcardfs.enabled (0 here in
 * /vendor/build.prop, and not re-enableable — it gates a dozen other vold paths). So
 * the module restores behaviour AOSP intended.
 *
 * ------------------------- why the ELF tables
 *
 * SetDefaultAcl's whole job is one setxattr call, and across all of vold-16 setxattr
 * appears EXACTLY ONCE (Utils.cpp:195, inside SetDefaultAcl; getxattr never). So
 * "setxattr does nothing" == "SetDefaultAcl returns OK without writing the ACL".
 * setxattr is an imported libc symbol, so its address comes from vold's OWN ELF
 * tables, not a build-specific byte signature (the old patch anchored on
 * `tbz w0,#0,<ACL block>` in .text).
 *
 * ------------------------- how it is found
 *
 * 1. Load base: /proc/<pid>/maps, first entry with offset=0 (the PIE image is mapped
 *    whole, so ELF addresses are offsets from this base).
 * 2. GOT slot: in PT_DYNAMIC take DT_JMPREL (.rela.plt); find the R_AARCH64_JUMP_SLOT
 *    whose .dynsym index names the UNDEF symbol "setxattr"; its r_offset is the VA.
 * 3. Stub: one pass over executable PT_LOADs collects every canonical
 *    adrp x16,<page> / ldr x17,[x16,#imm12*8] / add x16,x16,#imm12*8 / br x17
 *    whose ldr lands exactly on that GOT slot (self-check that the stub is
 *    setxattr's). The relocation index is NOT used: lld puts two service entries
 *    before the first symbol, so "rela.plt[i] -> plt+16*(i+1)" is off by one (on
 *    device setxattr is rela.plt[185], its stub #187). --selftest checks the mapping is one-to-one for ALL symbols.
 * 4. Repeat run / --check: the patch overwrote `ldr`, so the stub is no longer
 *    findable via the GOT slot. But .plt layout is linear: for any other pair
 *    "relocation i -> stub v", C = v - 16*i is constant (lld's numbering shift).
 *    Derive C from foreign pairs; setxattr's stub is C + 16*i_setxattr (v2.9.0 hardcoded this C as 0x54ea0).
 * 5. Patch writes 8 bytes: `mov w0,#0` (0x52800000) then `ret` (0xd65f03c0). Eight,
 *    not sixteen: the /proc/<pid>/mem write is a memcpy into the page and 8 bytes at
 *    an aligned address land as one word — atomically; the trailing `add`/`br` stay
 *    but are unreachable. Return 0, not -1, because FsCrypt.cpp:1027 checks `if (ret != android::OK) return false;`.
 *
 * The .plt page is file-mapped r-x (RELRO covers only .got/.got.plt, later), so writing
 * via /proc/<pid>/mem triggers COW: only the process's private copy changes. The patch
 * lives in memory only, so it must be reinstalled after every boot; a fresh vold takes path 3.
 *
 * ------------------------- which releases
 *
 * The resolver is version-independent by construction — it reads vold's own tables, so
 * nothing here changes between 11 and 17 — but the EXPECTATION is not something to
 * assume. android_ver.h names the releases this was validated on and the AOSP site that
 * writes the ACL the patch disarms (11/12/12L/13: vold-<n>/Utils.cpp:192,
 * vold-14/15/16/17: :195/:195/:195/:196); the
 * release in force is printed with it, and one not in the table is marked as borrowing
 * the newest profile rather than being verified. --sdk overrides the detection, for
 * looking at an image that is not this device's. The refusal below is NOT version-
 * dependent: whatever the table says, exactly one call site or refuse.
 *
 * ============================== when it refuses
 *
 * Two premises are checked; if EITHER fails the tool refuses (code 2) and writes
 * NOTHING — refusing beats a false success that leaves the race while the log denies
 * it. (1) setxattr is called EXACTLY ONCE (--dry-run prints "calls N"); 0 calls would
 * report success changing nothing, >1 would disarm unrelated code. (2) Stubs are 16-byte
 * spaced and shaped as above (lld without BTI/PAC); with -mbranch-protection=bti they are
 * 32 bytes and C stops being constant, caught on the second pair.
 *
 * Return codes (--file answers --check/--dry-run as on a live process and is never
 * modified): 0 patch present; 1 vold absent or --check saw an intact stub; 2 could not parse
 * the ELF, find the symbol or the stub; 3 could not write.
 *
 * /proc/<pid>/mem needs PTRACE_MODE_ATTACH on vold: invoke the tool as ONE simple
 * su -c command (a compound command lands in the shell domain, which may not ptrace vold;
 * a redirect does not change the domain). Build: cc -std=c11 -Oz -o vold-noacl vold-noacl.c
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

#include "android_ver.h"

#define TARGET_SYM "setxattr"

/* Bytes written into the stub: mov w0, #0 ; ret */
#define PATCH_MOV0  0x52800000u
#define PATCH_RET   0xd65f03c0u
static const uint32_t PATCH_WORDS[2] = { PATCH_MOV0, PATCH_RET };

#define EXIT_OK         0
#define EXIT_NO_VOLD    1
#define EXIT_NO_RESOLVE 2
#define EXIT_NO_WRITE   3

static bool g_quiet = false;

static void info(const char *fmt, ...) {
    if (g_quiet) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("vold-noacl: ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
    /* stdout may be a log while stderr is not; flush so lines don't interleave. */
    fflush(stdout);
}

static void warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    /* Whole line to stderr: mixing a stderr prefix with stdout text splits lines under `2>&1`. */
    fputs("vold-noacl: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

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

/* Already-patched stub: mov w0,#0 / ret / add / br — `ldr` is gone, recognised by
 * the surviving `add` and `br`. */
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

typedef struct {
    uint64_t *target;
    uint64_t *va;
    size_t    n;
    size_t    cap;
    uint64_t *patched;
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

/* One pass over executable PT_LOADs: collect canonical stubs, note patched ones. */
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

typedef struct {
    uint64_t got_slot;
    uint64_t stub_va;
    uint32_t sym_index;
    int      reloc_index;
    int      call_sites;
    int      plt_delta;          /* C/16 derived from the .plt layout */
    bool     already_patched;
} Resolved;

/* Count `bl`/`b` targets pointing at the stub — report how many vold sites call it. */
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
            if (imm & 0x02000000) imm |= (int32_t)0xfc000000u;  /* sign-extend 26 bits */
            uint64_t pc = p->p_vaddr + off;
            if ((uint64_t)((int64_t)pc + (int64_t)imm * 4) == stub_va) hits++;
        }
        free(buf);
    }
    return hits;
}

/* Is the symbol's address taken (any .rela.dyn reloc)? Then one patched stub won't
 * cover that use. */
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

/* .plt layout: for every relocation whose stub was found, C = va - 16*i must be
 * identical. Returns C/16 or -1. */
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

    /* 1. The symbol's relocation: index and GOT slot. */
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

    /* 2. All stubs in one pass. */
    Stubs st;
    if (collect_stubs(s, &st) != 0) {
        free(rel);
        return EXIT_NO_RESOLVE;
    }

    /* 3. .plt layout from foreign pairs — works even when the symbol's own ldr is gone. */
    r->plt_delta = plt_delta(&st, rel, nrel);
    free(rel);
    if (r->plt_delta < 0) {
        stubs_free(&st);
        return EXIT_NO_RESOLVE;
    }

    /* 4. The stub: direct GOT-slot lookup (fresh vold), else the derived layout. */
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

    if (want_call_sites) {
        r->call_sites = count_call_sites(s, r->stub_va);

        /* The patch's PREMISE, not the layout: it only makes sense because vold calls
         * setxattr once, from SetDefaultAcl. 0 calls would report success changing
         * nothing; >1 would also break unrelated code — both refuse. Works on an
         * already-patched vold: `bl` in .text is untouched, only the stub's `ldr` is gone. */
        if (r->call_sites != 1) {
            warn("%s: \"%s\" вызывается %d раз(а), а ожидался ровно один — "
                 "отказываюсь (патч рассчитан на единственный вызов из "
                 "SetDefaultAcl)", s->label, TARGET_SYM, r->call_sites);
            stubs_free(&st);
            return EXIT_NO_RESOLVE;
        }
    }
    stubs_free(&st);
    return EXIT_OK;
}

/* Require exactly one stub per JUMP_SLOT relocation and distinct stubs for distinct
 * symbols, so no arithmetic like "rela.plt[i] -> plt+16*(i+1)" is needed. On a patched
 * image a patched stub has no `ldr` and is not decoded, so misses are tolerated only
 * if their count equals the number we patched. */
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

    bool explained = ((size_t)missing == npatched);
    bool ok = (matched + missing == (int)n) && ambiguous == 0 &&
              badtype == 0 && dupes == 0 && explained;
    info("%s: сопоставление %s", s->label,
         ok ? (npatched ? "взаимно однозначное (кроме пропатченных)"
                        : "взаимно однозначное")
            : "НЕПОЛНОЕ");
    return ok ? EXIT_OK : EXIT_NO_RESOLVE;
}

static void usage(void) {
    fputs("usage: vold-noacl [--wait SEC] [--pid PID] [--check] [--dry-run]\n"
          "                  [--file ELF] [--selftest] [--sdk N] [--quiet]\n",
          stderr);
}

int main(int argc, char **argv) {
    int  wait_sec = 0;
    long pid_opt = -1;
    long sdk_opt = -1;
    bool check = false, dry_run = false, self = false;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wait") && i + 1 < argc) {
            wait_sec = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--pid") && i + 1 < argc) {
            pid_opt = strtol(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--sdk") && i + 1 < argc) {
            sdk_opt = strtol(argv[++i], NULL, 10);
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

    /* The release in force: this system's, or --sdk for an image that is not this
     * device's. It decides what the run is compared against and how the log
     * reads — the resolution below reads the target's own tables either way,
     * which is why 11 through 17 take the identical path. */
    UnfusePick vp = unfuse_pick(sdk_opt > 0 ? (int)sdk_opt : unfuse_sdk());
    char vbuf[192];
    unfuse_ver_str(&vp, vbuf, sizeof(vbuf));
    info("версия: %s; default-ACL пишет %s", vbuf, vp.v->vold_acl);

    Src s;
    memset(&s, 0, sizeof(s));
    s.fd = -1;

    char exe[4096] = {0};

    if (file) {
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

                /* The file is never modified, so only a live process is worth writing.
                 * But --check/--dry-run must answer identically here and there, or an
                 * offline check would lie about "no patch". */
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
    /* exe is filled by identification (or empty for --pid); it is both filter and output. */
    int lb = find_load_base(pid, exe, &base, exe, sizeof(exe));
    if (lb == -1) {
        warn("pid %d: /proc/%d/maps не читается — скорее всего нет "
             "PTRACE_MODE_READ к этому процессу", (int)pid, (int)pid);
        return EXIT_NO_RESOLVE;
    }
    if (lb != 0) {
        warn("pid %d: в /proc/%d/maps нет ни одного отображения с offset=0 и "
             "путём — базу загрузки определить нечем", (int)pid, (int)pid);
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

    /* Read the whole stub: 8 bytes suffice to compare, but decode_stub also checks `add` and `br`. */
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

    /* Before writing, confirm the stub is adrp+ldr+add+br and its ldr reads our GOT slot. */
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
