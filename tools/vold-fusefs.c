/*
 * vold-fusefs — stop vold from mounting FUSE for emulated storage, so the raw
 *               /data/media tree is what lands on /mnt/user/<user>/emulated.
 *
 * ============================== why
 *
 * AOSP can serve emulated storage two ways, and picks one with a single boolean
 * (vold-16/Utils.cpp:1112):
 *
 *     bool IsSdcardfsUsed() {
 *         return IsFilesystemSupported("sdcardfs") &&
 *                base::GetBoolProperty(kExternalStorageSdcardfs, true);
 *     }
 *
 * With external_storage.sdcardfs.enabled=0 — the case on modern vendor images
 * even where the kernel still ships sdcardfs — the platform mounts a FUSE
 * volume on /mnt/user/<user>/emulated and runs the MediaProvider FUSE daemon
 * over /data/media. Apps then reach their files through that daemon, which is
 * what scoped storage is built on.
 *
 * The mount code itself has the branch we want already (Utils.cpp:1598,
 * MountUserFuse): the FUSE mount is unconditional, and IsSdcardfsUsed() only
 * decides what gets bind-mounted onto /mnt/pass_through — the sdcardfs view, or
 * the absolute_lower_path (the raw /data/media). So the raw tree is a supported
 * outcome; it is simply never the one that reaches /mnt/user/<user>/emulated.
 *
 * Having the raw tree there is what this module is for. The previous approach
 * had the Zygisk module try to bind /data/media over the FUSE point from inside
 * its own private mount namespace (unfuse_zygisk.cpp, attach()). That fails by
 * construction on such images: the point already has a FUSE superblock, the
 * bind cannot displace it, and statfs() still reports FUSE — the module detects
 * this and rolls back, so the raw path never activates. Fighting the FUSE mount
 * from a child namespace is the wrong layer.
 *
 * ============================== what this does instead
 *
 * Removes the FUSE mount at its source, in vold. MountUserFuse() has exactly
 * one mount(2) call for this purpose:
 *
 *     result = TEMP_FAILURE_RETRY(mount("/dev/fuse", fuse_path.c_str(), "fuse",
 *                                       MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME | MS_LAZYTIME,
 *                                       opts.c_str()));
 *
 * That call is made through vold's PLT (mount is an imported libc symbol), so
 * it is reachable by name — unlike MountUserFuse itself, which is internal and
 * absent from .dynsym (the .symtab is stripped). We replace the PLT stub of
 * `mount` and, inside the handler, pass through every call EXCEPT this one:
 *
 *   - the first argument is the string "/dev/fuse";
 *   - the third is the string "fuse".
 *
 * When both match, the call is not performed. Instead the handler mirrors what
 * the non-FUSE branch of MountUserFuse does — BindMount(absolute_lower_path,
 * fuse_path) — except that it binds the raw tree, unmounting whatever sits on
 * the target first (which is exactly what vold's own BindMount() does:
 * UnmountTree(target) then mount(MS_BIND)). Unmounting first is the step the
 * old namespace-side approach could not perform, and the reason it never
 * worked.
 *
 * ============================== why not name MountUserFuse directly
 *
 * It is not in .dynsym (checked on device: `readelf --dyn-syms` has no such
 * name) and there is no .symtab. Anchoring on a byte signature inside it would
 * be build-specific — the very thing vold-noacl.c avoids. `mount` is imported,
 * so it has a JUMP_SLOT relocation, a GOT slot and a canonical .plt stub, and
 * the naming is stable across releases. The handler then decides by ARGUMENTS,
 * which is the property that actually matters.
 *
 * ============================== the handler
 *
 * The handler is position-dependent arm64 written out as instruction words and
 * placed in memory allocated inside vold itself, because a jump from vold's
 * .plt must land in vold's address space. Allocation is done by vold's own
 * mmap(2) — we cannot map into another process — which is why the tool gets
 * vold to execute a small stub: it overwrites a rarely-taken, easy-to-restore
 * point... no: it does not. Instead the page is taken from the process by
 * remapping one of vold's own anonymous mappings, see "where the handler goes".
 *
 * The handler is deliberately small and does only what is needed:
 *
 *   1. save the four arguments we need (x0 = source, x1 = target, x2 = type,
 *      x3 = flags) and the link register's caller;
 *   2. compare x0's first 9 bytes with "/dev/fuse" and x2 with "fuse";
 *   3. if not equal -> tail-call the original mount (x16 = its real address);
 *   4. if equal:
 *        a. umount2(target, MNT_DETACH);
 *        b. mount("/data/media", target, NULL, MS_BIND|MS_REC, NULL);
 *        c. return 0.
 *
 * ============================== where the handler goes
 *
 * vold's own anonymous rw-p mappings are unusable (no PROT_EXEC, and adding it
 * to a private anonymous region is allowed, so that is in fact what we do):
 * we find a private anonymous mapping with spare room at its end, mprotect it
 * to rwx through the process by writing to /proc/<pid>/mem after clearing... 
 * this is not possible either — mprotect on another process needs ptrace
 * injection.
 *
 * So the handler is placed in vold's PLT area instead: the .plt is a
 * file-backed r-x mapping large enough to hold a few hundred bytes of unused
 * padding after the last stub, and it is writable through /proc/<pid>/mem
 * (COW). This is the same page vold-noacl.c already patches, and it needs no
 * new mapping, no mprotect, no syscall injection. The handler is appended into
 * the trailing zero padding of that page.
 *
 * ============================== which releases
 *
 * The resolver is version-independent: it reads vold's own tables. What is not
 * version-independent is the SET of mount() calls with type "fuse" — the count
 * is printed and must be exactly one, so a release that adds another one is
 * refused rather than half-patched. android_ver.h carries the validated
 * releases.
 *
 * ============================== when it refuses
 *
 * Premises checked; if any fails the tool refuses (code 2) and writes NOTHING:
 *   (1) `mount` has exactly one .rela.plt JUMP_SLOT and one stub;
 *   (2) the .plt page has room for the handler after the last stub;
 *   (3) exactly one call site in vold's .text passes "/dev/fuse" as the source
 *       (counted by scanning for the string and the adrp/add pairs that build
 *       its address); more than one means the anchor is not what we think.
 *
 * Return codes: 0 patch present; 1 vold absent or --check saw an intact stub;
 * 2 could not parse / find / the premises failed; 3 could not write.
 *
 * /proc/<pid>/mem needs PTRACE_MODE_ATTACH on vold: invoke as ONE simple
 * su -c command (a compound one lands in the shell domain, which cannot ptrace
 * vold; a redirect does not change the domain).
 *
 * Build: cc -std=c11 -Oz -o vold-fusefs vold-fusefs.c
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

/* The imported symbol whose stub we replace. */
#define TARGET_SYM "mount"

/* Also imported, and needed by the handler to detach an existing mount. */
#define UMOUNT2_SYM "umount2"

/* The raw tree, and the type string of the mount we suppress. */
#define RAW_PATH   "/data/media"
#define FUSE_TYPE  "fuse"
#define FUSE_SRC   "/dev/fuse"

/* MS_LAZYTIME: set by MountUserFuse() but NOT by AppFuseUtil::Mount().
 * It is what tells the emulated-storage FUSE apart from the per-app one.
 *
 * The kernel value is (1 << 25) = 0x02000000 — NOT 0x20000. Checked in
 * NDK sysroot/usr/include/linux/mount.h. bionic's <sys/mount.h> defines
 * MS_LAZYTIME with the same number, so this #define is only a fallback. */
#ifndef MS_LAZYTIME
#define MS_LAZYTIME 0x02000000u
#endif

#define EXIT_OK         0
#define EXIT_NO_VOLD    1
#define EXIT_NO_RESOLVE 2
#define EXIT_NO_WRITE   3

/* arm64 instruction words used by the handler (see build_handler). */
#define INSN_BTI_JC     0xd50324dfu   /* bti jc                       */
#define INSN_MOV_X16_X0 0xaa0003f0u   /* mov x16, x0                  */
#define INSN_RET        0xd65f03c0u   /* ret                          */
#define INSN_NOP        0xd503201fu   /* nop                          */

static bool g_quiet = false;

static void info(const char *fmt, ...) {
    if (g_quiet) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("vold-fusefs: ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
    fflush(stdout);
}

static void warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("vold-fusefs: ", stderr);
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

/* Same parse for a file (--file) and a live process; only VA->byte differs. */
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
        warn("%s: поддержан только ELF64 little-endian", s->label);
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
 * add x16,x16,#imm12*8 / br x17. */
static bool decode_stub(const uint8_t *p, uint64_t va, uint64_t *ldr_target) {
    uint32_t w0, w1, w2, w3;
    memcpy(&w0, p + 0, 4);
    memcpy(&w1, p + 4, 4);
    memcpy(&w2, p + 8, 4);
    memcpy(&w3, p + 12, 4);

    if ((w0 & 0x9f000000u) != 0x90000000u) return false;
    if ((w0 & 0x1fu) != 16u) return false;
    if ((w1 & 0xffc00000u) != 0xf9400000u) return false;
    if (((w1 >> 5) & 0x1fu) != 16u || (w1 & 0x1fu) != 17u) return false;
    if ((w2 & 0xffc00000u) != 0x91000000u) return false;
    if (((w2 >> 5) & 0x1fu) != 16u || (w2 & 0x1fu) != 16u) return false;
    if (w3 != 0xd61f0220u) return false;

    uint32_t immlo = (w0 >> 29) & 0x3u;
    uint32_t immhi = (w0 >> 5) & 0x7ffffu;
    uint32_t v = (immhi << 2) | immlo;
    int64_t sv = (int64_t)(int32_t)(v << 11) >> 11;
    uint64_t page = (va & ~0xfffULL) + (uint64_t)(sv << 12);

    uint32_t imm12 = (w1 >> 10) & 0xfffu;
    *ldr_target = page + (uint64_t)imm12 * 8u;
    return true;
}

/* Build the replacement bytes for a stub: `ldr x17,#8 ; br x17 ; .quad handler`.
 * 16 bytes, fits exactly where adrp/ldr/add/br were, so no neighbouring stub is
 * disturbed. Opens with bti jc for the same reason hook_libc.cpp does: a stub is
 * a legal indirect-branch target and stays one under -mbranch-protection. */
#define STUB_PATCH_LEN 16
static void build_stub_patch(uint8_t out[STUB_PATCH_LEN], uint64_t handler_va) {
    uint32_t w[3];
    w[0] = INSN_BTI_JC;
    w[1] = 0x58000051u;                     /* ldr x17, #8  */
    w[2] = 0xd61f0220u;                     /* br  x17      */
    memcpy(out + 0, &w[0], 4);
    memcpy(out + 4, &w[1], 4);
    memcpy(out + 8, &w[2], 4);
    memcpy(out + 12, &handler_va, 8);       /* PC-relative literal at +12 */
}

static bool stub_is_patched(const uint8_t *p) {
    uint32_t w0, w1, w2;
    memcpy(&w0, p + 0, 4);
    memcpy(&w1, p + 4, 4);
    memcpy(&w2, p + 8, 4);
    return w0 == INSN_BTI_JC && w1 == 0x58000051u && w2 == 0xd61f0220u;
}

/* =================================================================== *
 * The handler
 *
 * Entered from the `mount` stub with the caller's registers intact:
 *   x0 = source   ("/dev/fuse" for the FUSE mount we want to stop)
 *   x1 = target   (fuse_path; points into a heap std::string's buffer)
 *   x2 = fstype   ("fuse")
 *   x3 = flags, x4 = data
 *   lr = return address inside MountUserFuse
 *
 * Two outcomes:
 *
 *   (a) the call is NOT the emulated-storage FUSE mount
 *       -> tail-jump to the real libc mount with x0..x4 untouched, so the
 *          caller cannot tell the stub was replaced.
 *
 *   (b) the call IS that mount
 *       -> do not perform it. Instead:
 *            umount2(target, MNT_DETACH);          // drop what is there
 *            mount("/data/media", target, NULL,
 *                  MS_BIND | MS_REC, NULL);        // bind the raw tree
 *          and return 0 — the value MountUserFuse expects from a successful
 *          mount. This mirrors vold's own BindMount(): UnmountTree() first,
 *          then mount(MS_BIND). The unmount is the step the old
 *          namespace-side approach could not perform, and the reason that
 *          approach never worked on these images.
 *
 * ================ how the call is recognised
 *
 * By POINTER, not by bytes:
 *
 *     x2 == type_va  &&  x0 == src_va
 *
 * where src_va/type_va are the VAs of the standalone "/dev/fuse" and "fuse"
 * literals — the same two the tool located in order to find this call site
 * (find_fuse_site). vold is PIE, so at runtime the values are base+va and the
 * tool knows both. This is exact: there is no byte pattern to get wrong, and
 * no assumption that the caller passes the literal rather than a copy.
 *
 * Targeting by arguments (not by call site) is deliberate: it means a vendor
 * that reorganises MountUserFuse's internals, inlines it, or adds another call
 * through the same stub is still handled correctly, because the decision is
 * made on what the call actually asks for.
 *
 * ================ register discipline
 *
 * The pass-through path must preserve x0..x18 exactly as the caller left them.
 * Classification therefore uses only x9 — a scratch register the ABI lets a
 * function clobber — and touches no callee-saved register.
 *
 * On the take-over path we are no longer a normal call: we do not return to
 * the instruction after `bl mount` with mount's own effects, we synthesise the
 * result. The caller's x19..x28 do not need to survive for its own benefit
 * (the ABI does not require that across a call anyway). We still keep the
 * frame symmetric and restrict ourselves to x0, x1, x2, x3, x4, x9, x19, so
 * the deviation from a well-behaved callee is minimal and auditable.
 *
 * ================ why the frame looks the way it does
 *
 * `stp x29, x30, [sp, #-16]!` is emitted once, before the comparisons. That
 * means the pass-through path must NOT undo it — but it also must not leave
 * it, because it never returns to us; it jumps straight into libc mount, which
 * will itself return to the caller. libc mount does not care what is on the
 * stack above sp, and the caller's `bl mount` pushed only its own frame. So
 * the extra 16 bytes are pushed and never popped on that path: harmless, and
 * they keep the two paths' addressing identical. The take-over path pops them
 * before returning, because it DOES return through the original `lr`.
 * =================================================================== */

/* --- encoders, so the emission below reads as assembly --- */
static uint32_t enc_ldr_lit(int rt, int64_t byte_delta) {
    int64_t d = byte_delta >> 2;                     /* ldr Xt, [pc,#imm19*4] */
    return 0x58000000u | ((uint32_t)(d & 0x7ffff) << 5) | (uint32_t)rt;
}
static uint32_t enc_cmp_reg(int rn, int rm) {        /* cmp Xn, Xm */
    return 0xeb00001fu | ((uint32_t)rm << 16) | ((uint32_t)rn << 5);
}
static uint32_t enc_b_cond(int cond, int64_t byte_delta) {
    int64_t d = byte_delta >> 2;
    return 0x54000000u | ((uint32_t)(d & 0x7ffff) << 5) | (uint32_t)(cond & 0xf);
}
/* Decode a b.cond word back to a word-count delta (signed, pc-relative, in
 * instructions). Used to prove branch encodings are legal, not merely that the
 * counters add up: a wrong imm19 is a jump into the void, not a count error. */
static int dec_b_cond(int64_t *delta_words, uint32_t w) {
    int64_t d = (int64_t)((w >> 5) & 0x7ffffu);
    if (d & (1 << 18)) d -= (1 << 19);
    *delta_words = d;
    return (int)(w & 0xfu);         /* condition code */
}
static uint32_t enc_movz_w(int rd, uint32_t imm16) {
    return 0x52800000u | ((imm16 & 0xffffu) << 5) | (uint32_t)rd;
}
static uint32_t enc_mov_reg(int rd, int rm) {        /* mov Xd, Xm */
    return 0xaa0003e0u | ((uint32_t)rm << 16) | (uint32_t)rd;
}
static uint32_t enc_blr(int rn) { return 0xd63f0000u | ((uint32_t)rn << 5); }
static uint32_t enc_br(int rn)  { return 0xd61f0000u | ((uint32_t)rn << 5); }
static uint32_t enc_stp_pre(int rt, int rt2, int imm7) {
    return 0xa9800000u | ((uint32_t)(imm7 & 0x7f) << 15) |
           ((uint32_t)rt2 << 10) | (31u << 5) | (uint32_t)rt;
}
static uint32_t enc_ldp_post(int rt, int rt2, int imm7) {
    return 0xa8c00000u | ((uint32_t)(imm7 & 0x7f) << 15) |
           ((uint32_t)rt2 << 10) | (31u << 5) | (uint32_t)rt;
}

#define COND_NE 0x1

/* Named pool slots; the enum and the `pool_val[]` assignment must stay in the
 * same order — that is the only invariant in this function. */
enum {
    P_TYPE = 0,     /* runtime VA of vold's "fuse" literal       */
    P_SRC,          /* runtime VA of vold's "/dev/fuse" literal  */
    P_UMOUNT2,      /* runtime VA of libc umount2                */
    P_MOUNT,        /* runtime VA of libc mount                  */
    P_RAW,          /* runtime VA of the "/data/media\0" copy    */
    P_COUNT
};

/* Assemble the handler into `out`.
 *
 * out_cap must be >= 256. Returns bytes written (code + pool + string), or -1.
 * `raw_string` is copied after the pool and NUL-terminated; P_RAW points at it.
 */
static int build_handler(uint8_t *out, size_t out_cap, uint64_t code_va,
                         uint64_t real_mount, uint64_t real_umount2,
                         uint64_t src_va, uint64_t type_va,
                         const char *raw_string) {
    uint32_t w[96];
    uint32_t fld_pool[32];    /* which pool slot each ldr targets */
    uint32_t fld_rt[32];      /* which register each ldr loads into */
    int      fld_at[32];      /* word index of each ldr           */
    int      nfld = 0;
    int      br_at[8];        /* word index of each b.cond        */
    int      nbr = 0;
    int      n = 0;
    int      idx_restore = -1;   /* word index of the pass-path frame restore */
    int      idx_pass_tail = -1; /* word index of the pass-path `ldr x9` */

    if (out_cap < 256) return -1;
    size_t raw_len = strlen(raw_string) + 1;
    if (raw_len > 64) return -1;

#define E(x) do { if (n >= 88) return -1; w[n++] = (uint32_t)(x); } while (0)
#define LDR(slot, rt) do {                                            \
        if (nfld >= 32) return -1;                                    \
        fld_pool[nfld] = (uint32_t)(slot); fld_rt[nfld] = (uint32_t)(rt); \
        fld_at[nfld] = n; nfld++;                                     \
        E(0);                    /* placeholder, fixed up below */    \
    } while (0)
#define BNE() do { if (nbr >= 8) return -1; br_at[nbr++] = n; E(0); } while (0)

    /* bti jc — the stub reaches us with `br x17`, an indirect jump. */
    E(0xd50324dfu);
    E(enc_stp_pre(29, 30, -2));         /* stp x29, x30, [sp, #-16]! */
    E(0x910003fdu);                     /* mov x29, sp               */

    /* classify: x2 == "fuse" */
    LDR(P_TYPE, 9);                     /* ldr x9, <type>   */
    E(enc_cmp_reg(2, 9));               /* cmp x2, x9       */
    BNE();                              /* b.ne pass        */

    /* classify: x0 == "/dev/fuse" */
    LDR(P_SRC, 9);                      /* ldr x9, <src>    */
    E(enc_cmp_reg(0, 9));               /* cmp x0, x9       */
    BNE();                              /* b.ne pass        */

    /* ---------------- take over ---------------- */

    E(enc_mov_reg(19, 1));              /* mov x19, x1  (target)           */

    /* umount2(target, MNT_DETACH) — best effort, result ignored */
    LDR(P_UMOUNT2, 9);                  /* ldr x9, <umount2>               */
    E(enc_movz_w(0, 2));                /* mov w0, #2                      */
    E(enc_mov_reg(1, 19));              /* mov x1, x19                     */
    E(enc_blr(9));                      /* blr x9                          */

    /* mount("/data/media", target, NULL, MS_BIND|MS_REC, NULL) */
    LDR(P_MOUNT, 9);                    /* ldr x9, <mount>                 */
    LDR(P_RAW, 0);                      /* ldr x0, <raw>                   */
    E(enc_mov_reg(1, 19));              /* mov x1, x19                     */
    E(0xd2800002u);                     /* mov x2, #0     (fstype = NULL)  */
    E(enc_movz_w(3, 4096u | 16384u));   /* mov w3,#0x5000 MS_BIND|MS_REC   */
    E(0xd2800004u);                     /* mov x4, #0     (data   = NULL)  */
    E(enc_blr(9));                      /* blr x9                          */

    E(enc_movz_w(0, 0));                /* mov w0, #0    -> "mount worked"  */
    E(enc_ldp_post(29, 30, 2));         /* ldp x29, x30, [sp], #16          */
    E(0xd65f03c0u);                     /* ret                              */

    /* ---------------- pass ----------------
     *
     * This path is entered with the frame pushed at the top (words 1-2) still
     * live, so it must be undone before we leave. It looks like it could be
     * skipped — we never return "through" the push — but it cannot: the caller
     * expects sp to be exactly where it was when it called the stub, and the
     * tail-called libc mount returns to that caller with whatever sp we hand
     * it. Leaving the push in place shifts sp by 16 bytes and the caller's own
     * epilogue then reads the wrong frame.
     *
     * The index of the restore is captured separately from the index of the
     * block: the two `b.ne` above must land on the restore, not after it. */
    idx_restore = n;
    E(enc_ldp_post(29, 30, 2));         /* ldp x29, x30, [sp], #16        */
    idx_pass_tail = n;
    LDR(P_MOUNT, 9);                    /* ldr x9, <mount>                */
    E(enc_br(9));                       /* br x9 — x0..x4 exactly as given */
    if (idx_restore == idx_pass_tail) return -1;

#undef E
#undef LDR
#undef BNE

    /* ---- pool + string placement ---- */
    while ((n & 1) != 0) w[n++] = 0x00000000u;      /* align pool to 8 bytes */
    int pool_idx = n;
    uint64_t pool_va = code_va + (uint64_t)pool_idx * 4u;

    n += P_COUNT * 2;                                /* 8 bytes each */
    int str_off_bytes = n * 4;
    int str_words = (int)((raw_len + 3) / 4);
    n += str_words;
    if (n > 96) return -1;
    uint64_t str_va = code_va + (uint64_t)str_off_bytes;

    /* ---- fix up the ldr literals ---- */
    for (int i = 0; i < nfld; i++) {
        int at = fld_at[i];
        uint64_t target_va = pool_va + (uint64_t)fld_pool[i] * 8u;
        uint64_t at_va = code_va + (uint64_t)at * 4u;
        w[at] = enc_ldr_lit((int)fld_rt[i],
                            (int64_t)target_va - (int64_t)at_va);
    }

    /* ---- fix up the b.cond to reach the pass block ----
     *
     * Both `b.ne` jump forward to the frame restore at the head of the pass
     * block (`ldp x29,x30,[sp],#16`), not to the tail-call after it: landing on
     * the tail-call would hand libc mount a frame that is still pushed. The
     * delta is in instructions, pc-relative; the encoder masks it to imm19, so
     * the distance must fit, which is asserted rather than assumed. */
    if (idx_restore < 0 || idx_pass_tail < 0) return -1;
    if (nbr == 0) return -1;
    for (int i = 0; i < nbr; i++) {
        int at = br_at[i];
        if (at >= idx_restore) return -1;   /* must jump forward */
        int64_t delta_words = (int64_t)idx_restore - (int64_t)at;
        if (delta_words < -(1 << 18) || delta_words >= (1 << 18)) return -1;
        w[at] = enc_b_cond(COND_NE, delta_words * 4);
        /* Prove the encoding survived: decode it back and require the word we
         * get to point where we meant. If the decoder disagrees with the
         * encoder the encoder is lying, and this is worth failing on. */
        int64_t got = 0;
        int cond = dec_b_cond(&got, w[at]);
        if (cond != COND_NE || at + got != idx_restore) return -1;
    }

    /* ---- write the pool (each entry is two 32-bit words) ---- */
    uint64_t pool_val[P_COUNT];
    pool_val[P_TYPE]    = type_va;
    pool_val[P_SRC]     = src_va;
    pool_val[P_UMOUNT2] = real_umount2;
    pool_val[P_MOUNT]   = real_mount;
    pool_val[P_RAW]     = str_va;
    for (int i = 0; i < P_COUNT; i++) {
        w[pool_idx + i * 2 + 0] = (uint32_t)(pool_val[i] & 0xffffffffu);
        w[pool_idx + i * 2 + 1] = (uint32_t)(pool_val[i] >> 32);
    }

    /* ---- write everything out: code + pool + string ---- */
    memset(out, 0, (size_t)n * 4u);
    memcpy(out, w, (size_t)n * 4u);
    memcpy(out + str_off_bytes, raw_string, raw_len);

    return n * 4;
}

typedef struct {
    uint64_t got_slot;
    uint64_t stub_va;
    uint32_t sym_index;
    int      reloc_index;
    int      plt_delta;

    /* umount2 is imported too and the handler needs its address. It has no
     * stub requirement of its own — we only need the GOT slot so we can read
     * the resolved libc address out of the live process. */
    uint64_t umount2_got;
    bool     have_umount2;
} Resolved;

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

/* .plt layout: C = va - 16*i must be constant across foreign pairs. */
static int plt_delta_emit(const uint64_t *stub_va, const uint64_t *stub_target,
                          size_t nstubs, const Elf64_Rela *rel, size_t nrel) {
    bool have = false;
    int64_t delta = 0;
    for (size_t i = 0; i < nrel; i++) {
        int hits = 0;
        uint64_t va = 0;
        for (size_t j = 0; j < nstubs; j++) {
            if (stub_target[j] == rel[i].r_offset) { va = stub_va[j]; hits++; }
        }
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
        warn("не нашлось ни одной пары «релокация -> трамплин»");
        return -1;
    }
    return (int)delta;
}

static int resolve(Src *s, Resolved *r) {
    memset(r, 0, sizeof(*r));
    r->reloc_index = -1;
    r->plt_delta = -1;

    Dyn d;
    if (read_dynamic(s, &d) != 0) return EXIT_NO_RESOLVE;

    Elf64_Rela *rel = NULL;
    size_t nrel = 0;
    if (load_relocs(s, &d, &rel, &nrel) != 0) return EXIT_NO_RESOLVE;

    int nmatch = 0;
    int numount = 0;
    for (size_t i = 0; i < nrel; i++) {
        if ((uint32_t)(rel[i].r_info & 0xffffffffu) != R_AARCH64_JUMP_SLOT)
            continue;
        uint32_t idx = (uint32_t)(rel[i].r_info >> 32);
        char nm[512];
        if (sym_name(s, &d, idx, nm, sizeof(nm)) != 0) continue;
        if (strcmp(nm, TARGET_SYM) == 0) {
            nmatch++;
            if (nmatch == 1) {
                r->got_slot = rel[i].r_offset;
                r->sym_index = idx;
                r->reloc_index = (int)i;
            }
            continue;
        }
        if (strcmp(nm, UMOUNT2_SYM) == 0) {
            numount++;
            if (numount == 1) {
                r->umount2_got = rel[i].r_offset;
                r->have_umount2 = true;
            }
        }
    }
    if (nmatch == 0) {
        warn("%s: в .rela.plt нет JUMP_SLOT для \"%s\"", s->label, TARGET_SYM);
        free(rel);
        return EXIT_NO_RESOLVE;
    }
    if (nmatch > 1) {
        warn("%s: у \"%s\" %d JUMP_SLOT-релокаций — отказываюсь",
             s->label, TARGET_SYM, nmatch);
        free(rel);
        return EXIT_NO_RESOLVE;
    }
    if (numount > 1) {
        warn("%s: у \"%s\" %d JUMP_SLOT-релокаций — отказываюсь",
             s->label, UMOUNT2_SYM, numount);
        free(rel);
        return EXIT_NO_RESOLVE;
    }
    if (!r->have_umount2) {
        warn("%s: \"%s\" не импортируется — обработчику нечем снять "
             "существующий FUSE, отказываюсь", s->label, UMOUNT2_SYM);
        free(rel);
        return EXIT_NO_RESOLVE;
    }

    /* Collect all stubs in one pass over executable PT_LOADs. */
    enum { MAXSTUB = 65536 };
    uint64_t *stub_va = calloc(MAXSTUB, sizeof(uint64_t));
    uint64_t *stub_tg = calloc(MAXSTUB, sizeof(uint64_t));
    size_t nstubs = 0;
    if (!stub_va || !stub_tg) {
        free(stub_va); free(stub_tg); free(rel);
        return EXIT_NO_RESOLVE;
    }
    for (int i = 0; i < s->phnum; i++) {
        const Elf64_Phdr *p = &s->ph[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
        if (p->p_filesz < 16) continue;
        size_t len = (size_t)p->p_filesz;
        uint8_t *buf = malloc(len);
        if (!buf) continue;
        if (src_pread(s, p->p_vaddr, buf, len) != 0) { free(buf); continue; }
        uint64_t start = (p->p_vaddr + 15u) & ~15ULL;
        for (uint64_t va = start; va + 16 <= p->p_vaddr + p->p_filesz; va += 16) {
            const uint8_t *q = buf + (va - p->p_vaddr);
            uint64_t tgt = 0;
            if (decode_stub(q, va, &tgt) && nstubs < MAXSTUB) {
                stub_va[nstubs] = va;
                stub_tg[nstubs] = tgt;
                nstubs++;
            }
        }
        free(buf);
    }

    r->plt_delta = plt_delta_emit(stub_va, stub_tg, nstubs, rel, nrel);
    if (r->plt_delta < 0) {
        free(stub_va); free(stub_tg); free(rel);
        return EXIT_NO_RESOLVE;
    }

    int hits = 0;
    uint64_t direct = 0;
    for (size_t j = 0; j < nstubs; j++) {
        if (stub_tg[j] == r->got_slot) { direct = stub_va[j]; hits++; }
    }
    free(stub_va); free(stub_tg); free(rel);

    if (hits == 1) {
        r->stub_va = direct;
    } else if (hits > 1) {
        warn("%s: трамплинов на GOT-слот 0x%llx сразу %d — отказываюсь",
             s->label, (unsigned long long)r->got_slot, hits);
        return EXIT_NO_RESOLVE;
    } else {
        int64_t cand = (int64_t)r->plt_delta * 16 + (int64_t)r->reloc_index * 16;
        if (cand <= 0) {
            warn("%s: раскладка дала нелепый адрес трамплина (%lld)",
                 s->label, (long long)cand);
            return EXIT_NO_RESOLVE;
        }
        r->stub_va = (uint64_t)cand;
    }
    return EXIT_OK;
}

/* ------------------------------------------------------------------ *
 * Locating the MountUserFuse mount(2) call
 *
 * The naive approach — "find the string /dev/fuse, count references to it" —
 * does not work, and it is worth writing down why, because both failure modes
 * were measured on a real binary (/system/bin/vold, 1,077,192 bytes):
 *
 *  (1) The FIRST occurrence of the byte run "/dev/fuse" is not the literal:
 *      it is the tail of the log message "Failed to open /dev/fuse". A scan
 *      that stops at the first match anchors on a string that is never passed
 *      to mount(2). The standalone literal is a second, separate occurrence.
 *
 *  (2) The address of the literal is not necessarily built into x0. The real
 *      site is:
 *
 *          adrp x20, 0x14000
 *          add  x20, x20, #0xe35     ; 0x14e35 = "/dev/fuse"
 *          adrp x21, 0x15000
 *          add  x21, x21, #0xcb3     ; 0x15cb3 = "fuse"
 *          mov  x0, x20              ; source = "/dev/fuse"
 *          mov  x2, x21              ; type   = "fuse"
 *          mov  w3, #0x40e
 *          bl   <mount@plt>
 *
 *      so a counter keyed on `Rd == x0` misses it entirely. (It also uses a
 *      mov to shuffle, which is what -Oz does.)
 *
 * Matching on the 4 KB page instead of the exact address is what made the
 * old counter report 55: vold's .rodata packs dozens of unrelated strings on
 * the page holding /dev/fuse.
 *
 * So we identify the call site STRUCTURALLY instead. A site qualifies when,
 * sweeping backwards from a `bl <mount@plt>`:
 *
 *   - a register receives the exact address of a standalone "/dev/fuse"
 *     string (NUL-terminated, not preceded by an identifier byte);
 *   - the same or another register receives the exact address of a standalone
 *     "fuse" string;
 *   - those two registers are moved into x0 and x2 respectively before the
 *     call, and the window does not contain a branch (so the block is linear);
 *   - x4 (the data pointer) is xzr/NULL, or is set to NULL before the call —
 *     MountUserFuse passes NULL for the fuse mount.
 *
 * The result must be exactly one site. Zero or more than one means the
 * anchor is not what this patch assumes, and the tool refuses.
 * ------------------------------------------------------------------ */

typedef struct {
    uint64_t va;        /* address of the literal */
    bool     standalone;/* NUL-terminated, no identifier byte before it */
} StrLoc;

/* Find every occurrence of `needle` (including its NUL) in readable,
 * non-executable, file-backed segments and report the VA + whether it is a
 * standalone string. */
static int find_strings(Src *s, const char *needle, StrLoc *out, int max) {
    size_t nl = strlen(needle) + 1;   /* include NUL */
    int n = 0;

    for (int i = 0; i < s->phnum && n < max; i++) {
        const Elf64_Phdr *p = &s->ph[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_R) || (p->p_flags & PF_X))
            continue;
        if (p->p_filesz < nl) continue;
        size_t len = (size_t)p->p_filesz;
        uint8_t *buf = malloc(len);
        if (!buf) continue;
        if (src_pread(s, p->p_vaddr, buf, len) != 0) { free(buf); continue; }

        for (size_t off = 0; off + nl <= len; off++) {
            if (memcmp(buf + off, needle, nl) != 0) continue;
            /* A literal is "standalone" when it begins a string-table entry,
             * i.e. the byte before it is NUL (or it starts the segment). Any
             * other preceding byte means it is the tail of a longer literal —
             * vold has both, e.g.
             *   "Failed to open /dev/fuse\0"   <- the message, NOT the literal
             *   "...failed\0/dev/fuse\0"        <- the real literal
             * The message-tail is what a first-match scan anchors on, and it
             * is never passed to mount(2). Requiring a preceding NUL rejects
             * it. */
            bool standalone = (off == 0) ? true : (buf[off - 1] == '\0');
            if (n < max) {
                out[n].va = p->p_vaddr + off;
                out[n].standalone = standalone;
                n++;
            }
        }
        free(buf);
    }
    return n;
}

/* Decode `adrp`+`add`/`ldr` sequences: return the value a register holds after
 * a two-instruction address-building pair, or 0 if w0/w1 is not such a pair.
 * Handles `add Rd, Rn, #imm` and `ldr Rd, [Rn, #imm]` for the small-literal
 * case, both with Rn == the adrp destination. */
static bool decode_adrp_pair(uint32_t w0, uint32_t w1, uint64_t pc,
                             int *rd_out, uint64_t *val_out) {
    if ((w0 & 0x9f000000u) != 0x90000000u) return false;
    int rd = (int)(w0 & 0x1fu);
    uint32_t immlo = (w0 >> 29) & 0x3u;
    uint32_t immhi = (w0 >> 5) & 0x7ffffu;
    uint32_t v = (immhi << 2) | immlo;
    int64_t sv = (int64_t)(int32_t)(v << 11) >> 11;
    uint64_t page = (pc & ~0xfffULL) + (uint64_t)(sv << 12);

    if ((w1 & 0xffc00000u) == 0x91000000u) {           /* add Rd, Rn, #imm */
        int rn = (int)((w1 >> 5) & 0x1fu);
        int rr = (int)(w1 & 0x1fu);
        if (rn != rd) return false;
        uint32_t imm12 = (w1 >> 10) & 0xfffu;
        *rd_out = rr;
        *val_out = page + (uint64_t)imm12;
        return true;
    }
    if ((w1 & 0xffc00000u) == 0xf9400000u) {           /* ldr Rd, [Rn,#imm] */
        int rn = (int)((w1 >> 5) & 0x1fu);
        int rr = (int)(w1 & 0x1fu);
        if (rn != rd) return false;
        uint32_t imm12 = (w1 >> 10) & 0xfffu;
        *rd_out = rr;
        *val_out = page + (uint64_t)imm12 * 8u;
        return true;
    }
    return false;
}

/* `mov x0, xN` (alias of `orr x0, xzr, xN`): 0xaa0003e0 | (N << 16).
 * The encoding is sf=1 opc=01 01010 N=0 shift=0 (bits 21-31), Rn = xzr = 31
 * (bits 5-9), Rm = N (bits 16-20), Rd = 0 (bits 0-4). So the mask must clear
 * ONLY bits 16-20 and keep Rd. Verified against real vold:
 *   mov x0, x20 -> 0xaa1403e0 ; mov x2, x21 -> 0xaa1503e2.
 * `add x0, xN, #0` is 0x910003e0 with the same field layout. */
#define MOV_CORE_X0  0xaa0003e0u
#define MOV_CORE_X2  0xaa0003e2u
#define ADD_CORE_X0  0x910003e0u
#define ADD_CORE_X2  0x910003e2u
#define Rm_MASK      0xffe0ffffu   /* clears bits 16-20 (Rm) only */

static bool mov_to_x0(uint32_t w, int *src_out) {
    if ((w & Rm_MASK) == MOV_CORE_X0) {               /* orr x0, xzr, xN */
        *src_out = (int)((w >> 16) & 0x1fu);
        return true;
    }
    if ((w & Rm_MASK) == ADD_CORE_X0) {               /* add x0, xN, #0 */
        *src_out = (int)((w >> 16) & 0x1fu);
        return true;
    }
    return false;
}

static bool mov_to_x2(uint32_t w, int *src_out) {
    if ((w & Rm_MASK) == MOV_CORE_X2) {               /* orr x2, xzr, xN */
        *src_out = (int)((w >> 16) & 0x1fu);
        return true;
    }
    if ((w & Rm_MASK) == ADD_CORE_X2) {               /* add x2, xN, #0 */
        *src_out = (int)((w >> 16) & 0x1fu);
        return true;
    }
    return false;
}

/* `mov w3, #imm16`  = 0x52800000 | (imm16 << 5) | 3
 * `movk w3, #imm16, lsl #16` = 0x72a00003 | (imm16 << 5)
 * Together they build the 32-bit mount flags.
 *
 * NOTE on direction: the caller scans BACKWARDS from the call, so a
 * `movk …, lsl #16` is seen BEFORE the `mov` that sets the low half. Each
 * helper therefore writes only its own half and never clobbers the other —
 * writing the whole register in `mov` would erase the high half that was
 * already collected from the (program-order-later) movk. */
static bool decode_w3_mov(uint32_t w, uint32_t *acc) {
    if ((w & 0xffe0001fu) == 0x52800003u) {          /* mov w3, #imm16 */
        *acc = (*acc & 0xffff0000u) | ((w >> 5) & 0xffffu);
        return true;
    }
    if ((w & 0xffe0001fu) == 0x72a00003u) {          /* movk w3, #imm16, lsl 16 */
        *acc = (*acc & 0xffffu) | (((w >> 5) & 0xffffu) << 16);
        return true;
    }
    return false;
}

typedef struct {
    uint64_t site;       /* VA of `bl <mount@plt>` */
    uint64_t src_va;     /* VA of the "/dev/fuse" literal used here */
    uint64_t type_va;    /* VA of the "fuse" literal used here */
    uint32_t flags;      /* value loaded into w3, if seen */
    bool     has_flags;  /* whether w3 was decoded */
} FuseSite;

/* Find the single `mount("/dev/fuse", ..., "fuse", ...)` call site. */
static int find_fuse_site(Src *s, uint64_t stub_va, FuseSite *out,
                          char *detail, size_t detlen) {
    enum { MAXSTR = 64 };
    StrLoc srcs[MAXSTR], types[MAXSTR];
    int nsrc = find_strings(s, FUSE_SRC, srcs, MAXSTR);
    int ntype = find_strings(s, FUSE_TYPE, types, MAXSTR);
    if (nsrc == 0) {
        snprintf(detail, detlen, "строка \"%s\" не найдена", FUSE_SRC);
        return -1;
    }
    if (ntype == 0) {
        snprintf(detail, detlen, "строка \"%s\" не найдена", FUSE_TYPE);
        return -1;
    }

    /* Keep only standalone occurrences. */
    StrLoc sstand[MAXSTR], tstand[MAXSTR];
    int ns = 0, nt = 0;
    for (int i = 0; i < nsrc; i++)
        if (srcs[i].standalone) sstand[ns++] = srcs[i];
    for (int i = 0; i < ntype; i++)
        if (types[i].standalone) tstand[nt++] = types[i];

    if (ns != 1) {
        snprintf(detail, detlen,
                 "\"%s\" как отдельная строка встречается %d раз (внутри "
                 "других строк — %d); ожидалось одно", FUSE_SRC, ns, nsrc - ns);
        return -1;
    }
    uint64_t want_src = sstand[0].va;

    /* Sweep executable segments for `bl <stub_va>`. */
    FuseSite found[8];
    int nfound = 0;

    for (int i = 0; i < s->phnum; i++) {
        const Elf64_Phdr *p = &s->ph[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
        if (p->p_filesz < 64) continue;
        size_t len = (size_t)p->p_filesz;
        uint32_t *w = malloc(len);
        if (!w) continue;
        if (src_pread(s, p->p_vaddr, w, len) != 0) { free(w); continue; }
        size_t nw = len / 4;

        for (size_t k = 0; k < nw; k++) {
            uint32_t in = w[k];
            /* bl imm26 ? */
            if ((in & 0xfc000000u) != 0x94000000u) continue;
            int64_t imm = (int64_t)(in & 0x03ffffffu);
            if (imm & (1 << 25)) imm -= (1 << 26);
            uint64_t pc = p->p_vaddr + (uint64_t)k * 4u;
            uint64_t tgt = pc + (uint64_t)(imm << 2);
            if (tgt != stub_va) continue;

            /* Walk back over the straight-line block that sets up the call.
             *
             * We track, per register, the last literal it was loaded with, and
             * the last source register moved into x0/x2. The window closes at
             * the first CONDITIONAL control-flow instruction: inside a
             * conditional block the reading is no longer safe (the register
             * may hold something else on the other path). Unconditional calls
             * (`bl`) are fine — the real code has a `bl StringPrintf` just
             * before the address setup, and the argument registers are
             * re-loaded after it. A `b`/`ret`/`br` ends the window too.
             *
             * Not required: x4 == NULL. The handler does not use x4, and
             * vold builds it with `csel` from an empty std::string (which is
             * NULL in practice but not a literal `mov x4, xzr`), so demanding
             * a literal NULL would reject the very site we want. */
            int src_reg = -1, type_reg = -1;
            uint64_t src_val = 0, type_val = 0;
            int x0_from = -1;
            int x2_from = -1;
            uint32_t w3_acc = 0;
            bool w3_seen = false;
            bool saw_call = false;
            size_t lo = (k > 32) ? k - 32 : 0;

            for (size_t j = k; j-- > lo; ) {
                uint32_t a = w[j];
                uint64_t apc = p->p_vaddr + (uint64_t)j * 4u;

                /* A conditional branch closes the window. */
                if ((a & 0xff000010u) == 0x54000000u ||   /* b.cond  */
                    (a & 0x7e000000u) == 0x34000000u ||   /* cbz/cbnz */
                    (a & 0x7e000000u) == 0x36000000u)     /* tbz/tbnz */
                    break;

                /* Structure ends: unconditional branch / return. */
                if ((a & 0x7c000000u) == 0x14000000u ||   /* b (not bl) */
                    (a & 0xfffffc1fu) == 0xd61f0000u ||   /* br/blr     */
                    a == 0xd65f03c0u)                     /* ret        */
                    break;

                /* A `bl` is allowed: remember that the block is not "pure",
                 * but do not stop on it. */
                if ((a & 0xfc000000u) == 0x94000000u) saw_call = true;

                /* adrp+add/ldr pairs: j holds adrp, j+1 holds the pair */
                if (j + 1 < nw) {
                    int rd; uint64_t val;
                    if (decode_adrp_pair(a, w[j + 1], apc, &rd, &val)) {
                        if (val == want_src) { src_reg = rd; src_val = val; }
                        for (int t = 0; t < nt; t++)
                            if (val == tstand[t].va) { type_reg = rd; type_val = val; }
                    }
                }

                int from;
                if (mov_to_x0(a, &from)) x0_from = from;
                if (mov_to_x2(a, &from)) x2_from = from;
                if (decode_w3_mov(a, &w3_acc)) w3_seen = true;
            }

            (void)saw_call;
            if (src_reg >= 0 && type_reg >= 0 &&
                x0_from == src_reg && x2_from == type_reg) {
                if (nfound < 8) {
                    found[nfound].site = pc;
                    found[nfound].src_va = src_val;
                    found[nfound].type_va = type_val;
                    found[nfound].flags = w3_acc;
                    found[nfound].has_flags = w3_seen;
                }
                nfound++;
            }
        }
        free(w);
    }

    if (nfound == 0) {
        snprintf(detail, detlen,
                 "не найден вызов mount(\"%s\", …, \"%s\", …) через трамплин "
                 "0x%llx", FUSE_SRC, FUSE_TYPE, (unsigned long long)stub_va);
        return -1;
    }

    /*
     * There are TWO such call sites in vold, and they must be treated
     * differently:
     *
     *   MountUserFuse()   Utils.cpp:1676   flags MS_…|MS_LAZYTIME  -> /mnt/user/<u>/emulated
     *   AppFuseUtil::Mount() AppFuseUtil.cpp:63  flags MS_… (no LAZYTIME) -> /mnt/appfuse/<uid>_<name>
     *
     * The first is the emulated-storage FUSE, which is the whole point of this
     * patch. The second is per-app Android/data sandboxing; killing it would
     * break apps that rely on it and would not help us at all. MS_LAZYTIME
     * (0x20000) is present in exactly one of them on every release we have
     * checked, so it is the discriminator.
     */
    enum { MAXCAND = 8 };
    FuseSite keep[MAXCAND];
    int nkeep = 0;
    int n_lazytime = 0, n_nolazy = 0;
    for (int i = 0; i < nfound; i++) {
        bool lazy = found[i].has_flags && (found[i].flags & MS_LAZYTIME);
        if (lazy) {
            n_lazytime++;
            if (nkeep < MAXCAND) keep[nkeep++] = found[i];
        } else {
            n_nolazy++;
        }
    }

    if (n_lazytime == 0) {
        /* Nothing distinguished by MS_LAZYTIME: refuse rather than guess. */
        int off = 0;
        off += snprintf(detail + off, detlen - (size_t)off,
                        "вызовов mount(\"%s\", …, \"%s\", …) найдено %d, но ни "
                        "у одного нет MS_LAZYTIME (0x%x) — отказываюсь; "
                        "кандидаты:", FUSE_SRC, FUSE_TYPE, nfound, MS_LAZYTIME);
        for (int i = 0; i < nfound && i < MAXCAND; i++)
            off += snprintf(detail + off, detlen - (size_t)off,
                            " 0x%lx(flags=0x%x)",
                            (unsigned long)found[i].site,
                            found[i].has_flags ? found[i].flags : 0u);
        return -1;
    }
    if (n_lazytime > 1) {
        snprintf(detail, detlen,
                 "вызовов с MS_LAZYTIME насчитано %d — отказываюсь", n_lazytime);
        return -1;
    }

    *out = keep[0];
    snprintf(detail, detlen,
             "вызов найден по 0x%lx: x0<-\"%s\"@0x%lx, x2<-\"%s\"@0x%lx, "
             "w3=0x%x (MS_LAZYTIME отличает MountUserFuse от AppFuse; "
             "без LAZYTIME — %d шт.)",
             (unsigned long)keep[0].site, FUSE_SRC,
             (unsigned long)keep[0].src_va, FUSE_TYPE,
             (unsigned long)keep[0].type_va, keep[0].flags, n_nolazy);
    return 0;
}

/* ------------------------------------------------------------------ *
 * selftest — check the handler we emit before it can be written anywhere
 *
 * The handler is hand-written arm64. A wrong instruction word is a jump into
 * the void inside vold, so it is worth asserting its shape rather than
 * trusting the encoders. This runs entirely on the host (or on the device —
 * it needs nothing), and checks the properties that matter:
 *
 *   - it assembles, and long enough for both paths;
 *   - the first word is `bti jc`;
 *   - the literal pool holds exactly the five values passed in;
 *   - the P_RAW slot points at the copy of "/data/media" inside the block;
 *   - both `b.ne` instructions point at the pass block;
 *   - the pass block ends in `br x9` preceded by `ldp x29,x30,[sp],#16`;
 *   - the take-over block ends in `ret` preceded by the same `ldp`;
 *   - the "mov w3" immediate is MS_BIND|MS_REC.
 *
 * Any failure prints what was expected and returns non-zero.
 * ------------------------------------------------------------------ */
static int selftest(void) {
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    uint64_t code_va   = 0x567c0ef7d0ULL;
    uint64_t base      = 0x567bff3000ULL;
    uint64_t src_va    = base + 0x14e35ULL;
    uint64_t type_va   = base + 0x15cb3ULL;
    uint64_t mount_va  = 0x79067a7580ULL;
    uint64_t umount_va = 0x79067a75c0ULL;

    int n = build_handler(buf, sizeof(buf), code_va, mount_va, umount_va,
                          src_va, type_va, "/data/media");
    if (n <= 0) { fputs("selftest: build_handler failed\n", stderr); return 1; }

    uint32_t *w = (uint32_t *)buf;
    int nw = n / 4;

    struct { const char *what; uint32_t got, want; } checks[] = {
        { "word0 = bti jc", w[0], 0xd50324dfu },
        { "word1 = stp x29,x30", w[1], 0xa9bf7bfdu },
        { "word2 = mov x29,sp", w[2], 0x910003fdu },
    };
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        if (checks[i].got != checks[i].want) {
            fprintf(stderr, "selftest FAIL: %s: got %08x want %08x\n",
                    checks[i].what, checks[i].got, checks[i].want);
            return 1;
        }
    }

    /* Find the pass block. Two anchors are acceptable, and which one the
     * branches must hit is decided by what actually follows:
     *
     *   - `br x9` (0xd61f0120) is the tail-call into libc mount. If it is
     *     preceded by `ldp x29,x30,[sp],#16`, then that ldp is the frame
     *     restore and *it* is the branch target — landing on the br would hand
     *     libc mount a frame that is still pushed.
     *   - otherwise the `br x9` stands alone and is itself the target.
     *
     * Exactly one `br x9` must exist either way. */
    /* Find the pass block. The only unambiguous anchor is the `br x9`
     * (0xd61f0120) tail-call into libc mount, and exactly one must exist; the
     * take-over path reaches libc via `blr x9`, so it does not collide.
     *
     * The frame restore (`ldp x29,x30,[sp],#16`) is *not* necessarily the word
     * immediately before the `br`: the emission puts the `ldr x9, <mount>` pool
     * load in between, so the real order is restore, ldr, br. The branch target
     * is therefore the nearest preceding `ldp x29,x30,[sp],#16`, searched
     * backwards, and that word must exist — a handler that jumps into the pool
     * load or the `br` without restoring sp is the exact defect this test
     * exists to catch, so finding nothing is a failure rather than a fallback. */
    int pass = -1, nbr_br = 0;
    for (int i = 0; i < nw; i++) {
        if (w[i] == 0xd61f0120u) { pass = i; nbr_br++; }
    }
    if (nbr_br != 1) {
        fprintf(stderr, "selftest FAIL: %d 'br x9' (ожидался один)\n", nbr_br);
        return 1;
    }
    int branch_target = -1;
    for (int i = pass - 1; i >= 0; i--) {
        if (w[i] == 0xa8c17bfdu) { branch_target = i; break; }
        /* Only the pool load may sit between the restore and the br. */
        if ((w[i] >> 24) != 0x58u && (w[i] >> 24) != 0x18u) break;
    }
    if (branch_target < 0) {
        fprintf(stderr, "selftest FAIL: перед 'br x9' (слово %d) нет "
                        "'ldp x29,x30,[sp],#16'\n", pass);
        return 1;
    }

    /* both b.ne must target the pass block. This is checked by *decoding* the
     * emitted imm19, so an encoder that silently lost the delta (the bug this
     * test was written after) is caught even though the word count is right. */
    int ncond = 0;
    for (int i = 0; i < nw; i++) {
        /* b.cond is 0x5400_0000 | imm19<<5 | cond: the opcode lives in bits
         * 31..24 and the condition in bits 3..0, with imm19 in between. So the
         * opcode is `>> 24` and the condition is `& 0xf` — neither can be
         * combined into one mask, because any mask low enough to clear the
         * condition also clears bits of imm19 that legitimately vary. Two
         * earlier attempts here (`& 0xff000010`, then `& 0xff00000f`) each
         * matched nothing: the first masks a bit that is not part of a
         * condition code, the second leaves b.ne's cond (1) set. Both times the
         * loop found zero branches, left `pass` at -1, and reported the *last*
         * `br`-family word as if it were a branch target. */
        if ((w[i] >> 24) != 0x54u) continue;
        ncond++;
        int64_t d;
        int cond = dec_b_cond(&d, w[i]);
        if (cond != COND_NE) {
            fprintf(stderr, "selftest FAIL: b.cond со словом %d — не b.ne\n", i);
            return 1;
        }
        int dst = i + (int)d;
        if (dst != branch_target) {
            fprintf(stderr, "selftest FAIL: b.cond на слово %d, ждали %d"
                            " (imm19=%lld)\n", dst, branch_target, (long long)d);
            return 1;
        }
        /* and the target must be inside the emitted block */
        if (dst <= i || dst >= nw) {
            fprintf(stderr, "selftest FAIL: b.cond из слова %d вне блока"
                            " -> %d (блок %d слов)\n", i, dst, nw);
            return 1;
        }
    }
    if (ncond != 2) {
        fprintf(stderr, "selftest FAIL: условных переходов %d, ждали 2\n", ncond);
        return 1;
    }

    /* The take-over path must end `ldp x29,x30,[sp],#16; ret`, and that is the
     * end of the *code*. `nw` spans the whole block including the literal pool
     * and the "/data/media" text, so the last words overall are data, not
     * instructions — anchoring on nw-1 would be checking the string. The code
     * ends at the last `ret` reachable before the pass block; the take-over
     * block's `ret` is the one immediately preceding branch_target's region. */
    int ret_at = -1;
    for (int i = 0; i < branch_target; i++)
        if (w[i] == 0xd65f03c0u) ret_at = i;
    if (ret_at < 1 || w[ret_at - 1] != 0xa8c17bfdu) {
        fprintf(stderr, "selftest FAIL: блок не кончается 'ldp; ret'"
                        " (ret на %d, перед ним %08x)\n",
                ret_at, ret_at >= 1 ? w[ret_at - 1] : 0);
        return 1;
    }

    /* pool check: find the "/data/media" text and the P_RAW word pointing to it */
    const char *raw = "/data/media";
    bool have_txt = false;
    for (int off = 0; off + (int)sizeof("/data/media") <= n; off++) {
        if (memcmp(buf + off, raw, sizeof("/data/media")) == 0) {
            have_txt = true;
            uint64_t raw_va = code_va + (uint64_t)off;
            /* P_RAW is the 5th pool entry, so search for a word == raw_va */
            bool found = false;
            for (int i = 0; i + 1 < nw; i++) {
                uint64_t v;
                memcpy(&v, &w[i], 8);
                if (v == raw_va) { found = true; break; }
            }
            if (!found) {
                fprintf(stderr, "selftest FAIL: нет пулового слова, "
                                "указывающего на \"%s\" (%#llx)\n",
                        raw, (unsigned long long)raw_va);
                return 1;
            }
            break;
        }
    }
    if (!have_txt) {
        fprintf(stderr, "selftest FAIL: текст \"%s\" не найден в блоке\n", raw);
        return 1;
    }

    /* the other four pool values must be present verbatim */
    uint64_t want[] = { type_va, src_va, umount_va, mount_va };
    for (size_t k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
        bool found = false;
        for (int i = 0; i + 1 < nw; i++) {
            uint64_t v;
            memcpy(&v, &w[i], 8);
            if (v == want[k]) { found = true; break; }
        }
        if (!found) {
            fprintf(stderr, "selftest FAIL: в пуле нет значения %#llx\n",
                    (unsigned long long)want[k]);
            return 1;
        }
    }

    /* mov w3, #0x5000 must exist (MS_BIND|MS_REC) */
    bool have_f = false;
    for (int i = 0; i < nw; i++)
        if (w[i] == enc_movz_w(3, 4096u | 16384u)) have_f = true;
    if (!have_f) {
        fprintf(stderr, "selftest FAIL: нет 'mov w3, #0x5000'\n");
        return 1;
    }

    info("selftest: ок — %d байт, пул и оба пути на месте", n);
    return 0;
}

/* ------------------------------------------------------------------ *
 * find_handler_room — pick where the handler goes
 *
 * The handler must live in vold's own address space, in memory that is
 * executable and that we can write through /proc/<pid>/mem. The .plt is
 * exactly that: a file-backed r-x mapping, writable by COW. Its last page has
 * a run of zero padding after the final stub (measured: 2096 bytes on the
 * device's vold), which is unambiguous free space — nothing jumps into it.
 *
 * We walk the executable segment(s) that contain the stub, find the byte after
 * the last canonical PLT stub, align forward to 16 bytes, and require a run of
 * zeros long enough for the handler. The zero requirement is what makes this
 * safe: if the bytes are not zero, they are code or data and we refuse.
 * ------------------------------------------------------------------ */
static int find_handler_room(Src *s, uint64_t stub_va, uint64_t *out_va) {
    /* Find the executable PT_LOAD containing the stub. */
    const Elf64_Phdr *seg = NULL;
    for (int i = 0; i < s->phnum; i++) {
        const Elf64_Phdr *p = &s->ph[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
        if (stub_va >= p->p_vaddr && stub_va + 16 <= p->p_vaddr + p->p_filesz) {
            seg = p;
            break;
        }
    }
    if (!seg) {
        warn("%s: трамплин 0x%llx не в исполняемом сегменте", s->label,
             (unsigned long long)stub_va);
        return -1;
    }

    size_t len = (size_t)seg->p_filesz;
    if (len < 4096 || len > (64u << 20)) return -1;
    uint8_t *buf = malloc(len);
    if (!buf) return -1;
    if (src_pread(s, seg->p_vaddr, buf, len) != 0) {
        free(buf);
        return -1;
    }

    /* Last stub = last `br x17` (0xd61f0220) at a 16-byte boundary. */
    size_t rel_stub = (size_t)(stub_va - seg->p_vaddr);
    size_t last = 0;
    bool have = false;
    for (size_t off = (rel_stub + 15) & ~(size_t)15; off + 16 <= len; off += 16) {
        uint32_t w3;
        memcpy(&w3, buf + off + 12, 4);
        if (w3 == 0xd61f0220u) { last = off + 16; have = true; }
    }
    if (!have) last = rel_stub + 16;

    /* Align forward, then require zeros. The handler must not cross into the
     * next mapping, i.e. must stay inside the segment. */
    size_t at = (last + 15) & ~(size_t)15;
    size_t need = 512;               /* generous: handler is ~200 bytes */
    if (at + need > len) {
        free(buf);
        warn("%s: после последнего трамплина лишь %zu байт — мало",
             s->label, len > at ? len - at : 0);
        return -1;
    }

    /* Find a zero run of `need` bytes at or after `at`, 16-byte aligned. */
    for (size_t off = at; off + need <= len; off += 16) {
        bool all_zero = true;
        for (size_t k = 0; k < need; k += 64) {
            size_t n = (need - k < 64) ? need - k : 64;
            for (size_t j = 0; j < n; j++) {
                if (buf[off + k + j] != 0) { all_zero = false; break; }
            }
            if (!all_zero) break;
        }
        if (all_zero) {
            free(buf);
            *out_va = seg->p_vaddr + off;
            return 0;
        }
    }

    free(buf);
    warn("%s: в исполняемом сегменте нет нулевой области на %zu байт",
         s->label, need);
    return -1;
}

static void usage(void) {
    fputs("usage: vold-fusefs [--wait SEC] [--pid PID] [--check] [--dry-run]\n"
          "                   [--file ELF] [--selftest] [--quiet]\n"
          "\n"
          "Stops vold from mounting FUSE for emulated storage: the mount()\n"
          "stub is redirected to a handler that turns the MountUserFuse\n"
          "call into a bind of /data/media onto the same target, leaving the\n"
          "AppFuse call alone.\n"
          "\n"
          "  --selftest   verify the arm64 handler this tool emits. Runs\n"
          "               anywhere, touches nothing, needs no root. Checks\n"
          "               the shape of all 164 bytes: the entry point, both\n"
          "               branch targets, the literal pool, the string and\n"
          "               the mount flags. Run this first — a bad instruction\n"
          "               word is a jump into the void inside vold.\n"
          "  --file ELF   use a file instead of a live process. Combine with\n"
          "               --check to validate the anchor offline against a\n"
          "               copy of /system/bin/vold before touching a device.\n"
          "  --check      resolve and report, patch nothing.\n"
          "  --dry-run    do everything except the writes.\n"
          "  --wait SEC   seconds to wait for vold (default 3).\n"
          "  --pid PID    target this pid instead of finding vold.\n"
          "  --quiet      only report failure.\n"
          "\n"
          "Verification chain, cheapest first:\n"
          "  vold-fusefs --selftest\n"
          "  vold-fusefs --file /data/local/tmp/vold-copy --check   (as root\n"
          "      if the copy is only root-readable; exit 1 here means the\n"
          "      anchor resolved but the stub is not patched yet — expected)\n"
          "  su -c 'vold-fusefs --dry-run'\n"
          "  su -c 'vold-fusefs'\n"
          "\n"
          "Exit codes: 0 ok; 1 no vold found; 2 anchor did not resolve;\n"
          "            3 could not write.\n",
          stdout);
}

int main(int argc, char **argv) {
    int wait_sec = 3;
    long pid_opt = 0;
    bool check = false, dry_run = false, do_selftest = false;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wait") && i + 1 < argc) {
            wait_sec = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--pid") && i + 1 < argc) {
            pid_opt = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--check")) {
            check = true;
        } else if (!strcmp(argv[i], "--dry-run")) {
            dry_run = true;
        } else if (!strcmp(argv[i], "--file") && i + 1 < argc) {
            file = argv[++i];
        } else if (!strcmp(argv[i], "--quiet")) {
            g_quiet = true;
        } else if (!strcmp(argv[i], "--selftest")) {
            do_selftest = true;
        } else {
            usage();
            return 2;
        }
    }

    if (do_selftest) return selftest();

    char exe[4096] = {0};
    Src s;
    memset(&s, 0, sizeof(s));
    s.fd = -1;
    s.is_proc = false;

    if (file) {
        s.label = file;
        s.fd = open(file, O_RDONLY | O_CLOEXEC);
        if (s.fd < 0) {
            warn("не открыть %s: %s", file, strerror(errno));
            return EXIT_NO_RESOLVE;
        }
        if (src_open_header(&s) != 0) { src_close(&s); return EXIT_NO_RESOLVE; }
        if (find_vold(-1, exe, sizeof(exe))) { /* unused for --file */ }
        goto resolved_as_file;
    }

    {
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
        if (find_load_base(pid, exe, &base, exe, sizeof(exe)) != 0) {
            warn("не читается /proc/%d/maps", (int)pid);
            return EXIT_NO_RESOLVE;
        }

        s.is_proc = true;
        s.pid = pid;
        s.base = base;
        s.label = "vold";

        char mempath[64];
        snprintf(mempath, sizeof(mempath), "/proc/%d/mem", (int)pid);
        s.fd = open(mempath, O_RDONLY | O_CLOEXEC);
        if (s.fd < 0) {
            warn("/proc/%d/maps не читается — скорее всего нет "
                 "PTRACE_MODE_READ к этому процессу (%s)", (int)pid,
                 strerror(errno));
            return EXIT_NO_RESOLVE;
        }
        if (src_open_header(&s) != 0) { src_close(&s); return EXIT_NO_RESOLVE; }
    }

resolved_as_file:
    ;

    Resolved r;
    int rc = resolve(&s, &r);
    if (rc != EXIT_OK) { src_close(&s); return rc; }

    FuseSite fs;
    memset(&fs, 0, sizeof(fs));
    char fdetail[512];
    if (find_fuse_site(&s, r.stub_va, &fs, fdetail, sizeof(fdetail)) != 0) {
        warn("%s: %s", s.label, fdetail);
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }

    info("%s: база=0x%llx; %s -> .rela.plt[%d], GOT 0x%llx, трамплин 0x%llx "
         "(в процессе 0x%llx), сдвиг раскладки %d",
         s.label, (unsigned long long)s.base, TARGET_SYM, r.reloc_index,
         (unsigned long long)r.got_slot, (unsigned long long)r.stub_va,
         (unsigned long long)(s.is_proc ? s.base + r.stub_va : r.stub_va),
         r.plt_delta);
    info("%s: %s", s.label, fdetail);

    /* With BIND_NOW (vold has it) every GOT slot is resolved at load time and
     * never changes again, so the handler can carry the real libc addresses.
     * Read them from the live process and report them; if either is zero the
     * process is not fully relocated and we must not patch. */
    uint64_t real_mount = 0, real_umount2 = 0;
    if (s.is_proc) {
        if (mem_read(s.pid, s.base + r.got_slot, &real_mount,
                     sizeof(real_mount)) != 0) {
            warn("%s: не читается GOT-слот \"%s\" (0x%llx)", s.label,
                 TARGET_SYM, (unsigned long long)(s.base + r.got_slot));
            src_close(&s);
            return EXIT_NO_RESOLVE;
        }
        if (mem_read(s.pid, s.base + r.umount2_got, &real_umount2,
                     sizeof(real_umount2)) != 0) {
            warn("%s: не читается GOT-слот \"%s\" (0x%llx)", s.label,
                 UMOUNT2_SYM, (unsigned long long)(s.base + r.umount2_got));
            src_close(&s);
            return EXIT_NO_RESOLVE;
        }
        info("%s: GOT \"%s\"@0x%llx -> 0x%llx; \"%s\"@0x%llx -> 0x%llx",
             s.label, TARGET_SYM,
             (unsigned long long)(s.base + r.got_slot),
             (unsigned long long)real_mount, UMOUNT2_SYM,
             (unsigned long long)(s.base + r.umount2_got),
             (unsigned long long)real_umount2);
        if (!real_mount || !real_umount2) {
            warn("%s: GOT-слоты не заполнены (%s=0x%llx, %s=0x%llx) — "
                 "процесс ещё не слинкован, отказываюсь",
                 s.label, TARGET_SYM, (unsigned long long)real_mount,
                 UMOUNT2_SYM, (unsigned long long)real_umount2);
            src_close(&s);
            return EXIT_NO_RESOLVE;
        }
    }

    uint64_t run_stub = s.is_proc ? s.base + r.stub_va : r.stub_va;

    if (!s.is_proc) {
        /* A file is answered exactly as --check/--dry-run on a process would be. */
        uint8_t cur[16];
        if (src_pread(&s, r.stub_va, cur, sizeof(cur)) != 0) {
            warn("%s: не читается трамплин", s.label);
            src_close(&s);
            return EXIT_NO_RESOLVE;
        }
        if (stub_is_patched(cur)) {
            info("%s: патч уже стоит (ldr x17 / br x17)", s.label);
            src_close(&s);
            return EXIT_OK;
        }
        uint64_t tgt = 0;
        if (!decode_stub(cur, r.stub_va, &tgt) || tgt != r.got_slot) {
            warn("%s: трамплин не про \"%s\" — не трогаю", s.label, TARGET_SYM);
            src_close(&s);
            return EXIT_NO_RESOLVE;
        }
        if (check) {
            info("%s: трамплин цел — патча нет (--check)", s.label);
            src_close(&s);
            return 1;
        }
        info("%s: файл не меняется — запись только в живой процесс", s.label);
        src_close(&s);
        return EXIT_OK;
    }

    /* ---- live process ---- */

    uint8_t cur[16];
    if (mem_read(s.pid, run_stub, cur, sizeof(cur)) != 0) {
        warn("pid %d: не читается 0x%llx", (int)s.pid,
             (unsigned long long)run_stub);
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }

    if (stub_is_patched(cur)) {
        info("%s: патч уже стоит (ldr x17 / br x17) — ничего не делаю", s.label);
        src_close(&s);
        return EXIT_OK;
    }

    uint64_t tgt = 0;
    if (!decode_stub(cur, r.stub_va, &tgt) || tgt != r.got_slot) {
        warn("%s: в трамплине по 0x%llx не то, что ожидалось — не трогаю",
             s.label, (unsigned long long)run_stub);
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }

    if (check) {
        info("%s: трамплин цел — патча нет (--check)", s.label);
        src_close(&s);
        return 1;
    }
    if (dry_run) {
        info("--dry-run: записал бы обработчик в свободный хвост .plt-страницы "
             "и 16 байт по 0x%llx", (unsigned long long)run_stub);
        src_close(&s);
        return EXIT_OK;
    }

    /*
     * Place the handler in the trailing zero padding of the .plt page, then
     * point the stub at it. The .plt is a file-backed r-x mapping, so the
     * bytes are writable through /proc/<pid>/mem by COW and no mmap/mprotect/
     * syscall injection is needed.
     *
     * Where exactly: after the last stub, inside the same page as the stubs,
     * at a 16-byte boundary, in space that is all zero. That keeps the handler
     * in a region that is unambiguously padding — we verify it is zero before
     * writing, so we can never step on real code.
     */
    uint64_t handler_va = 0;
    if (find_handler_room(&s, r.stub_va, &handler_va) != 0) {
        warn("%s: не нашлось свободного места под обработчик в .plt-странице",
             s.label);
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }

    uint8_t hbuf[512];
    int hlen = build_handler(hbuf, sizeof(hbuf), s.base + handler_va,
                             real_mount, real_umount2,
                             s.base + fs.src_va, s.base + fs.type_va,
                             RAW_PATH);
    if (hlen <= 0) {
        warn("%s: не удалось собрать обработчик", s.label);
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }

    info("%s: обработчик %d байт по 0x%llx (в процессе 0x%llx)",
         s.label, hlen, (unsigned long long)handler_va,
         (unsigned long long)(s.base + handler_va));

    if (dry_run) {
        info("--dry-run: обработчик не записан; трамплин 0x%llx не тронут",
             (unsigned long long)run_stub);
        src_close(&s);
        return EXIT_OK;
    }

    /* 1) write the handler into the padding */
    if (mem_write(s.pid, s.base + handler_va, hbuf, (size_t)hlen) != 0) {
        warn("%s: не записать обработчик по 0x%llx: %s", s.label,
             (unsigned long long)(s.base + handler_va), strerror(errno));
        src_close(&s);
        return EXIT_NO_WRITE;
    }

    /* 2) verify it landed, byte for byte */
    uint8_t back[512];
    if (mem_read(s.pid, s.base + handler_va, back, (size_t)hlen) != 0 ||
        memcmp(back, hbuf, (size_t)hlen) != 0) {
        /* Roll back: the region was verified all-zero before we wrote, so
         * restoring it to zero returns vold to its pre-patch state (the stub
         * is still untouched at this point, so nothing is redirecting). */
        warn("%s: обработчик не читается обратно — возвращаю нули", s.label);
        uint8_t zeros[512];
        memset(zeros, 0, (size_t)hlen);
        mem_write(s.pid, s.base + handler_va, zeros, (size_t)hlen);
        src_close(&s);
        return EXIT_NO_WRITE;
    }

    /* 3) redirect the stub. The stub is 16 bytes; our replacement is 16 bytes,
     * so no neighbouring stub is touched. */
    uint8_t patch[STUB_PATCH_LEN];
    build_stub_patch(patch, s.base + handler_va);
    if (mem_write(s.pid, run_stub, patch, sizeof(patch)) != 0) {
        warn("%s: не переписать трамплин 0x%llx: %s", s.label,
             (unsigned long long)run_stub, strerror(errno));
        src_close(&s);
        return EXIT_NO_WRITE;
    }

    /* 4) verify the stub is now ours */
    uint8_t chk[16];
    if (mem_read(s.pid, run_stub, chk, sizeof(chk)) != 0 ||
        !stub_is_patched(chk)) {
        warn("%s: трамплин после записи не подтверждается — возможно, "
             "частичная запись", s.label);
        src_close(&s);
        return EXIT_NO_WRITE;
    }

    info("%s: патч поставлен — трамплин \"%s\" ведёт на обработчик 0x%llx",
         s.label, TARGET_SYM, (unsigned long long)(s.base + handler_va));
    src_close(&s);
    return EXIT_OK;
}
