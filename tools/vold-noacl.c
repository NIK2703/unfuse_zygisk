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
 * The resolver is version-independent by construction: it reads vold's own tables, so
 * nothing here changes between 11 and 17 — and the tool consults no release table at
 * all. The AOSP site that writes the ACL the patch disarms was measured per release
 * (11/12/12L/13: vold-<n>/Utils.cpp:192, vold-14/15/16/17: :195/:195/:195/:196) and
 * that measurement is what android_ver.h carries; it is the Zygisk module's hooks
 * (src/hook_libc.cpp, via hooks_release()) that read it, because the module has a
 * target count to compare. Nothing here does: the refusal below is NOT
 * version-dependent — whatever the release, exactly one call site or refuse.
 *
 * ============================== when it refuses
 *
 * Two premises are checked; if EITHER fails the tool refuses (code 2) and writes
 * NOTHING — refusing beats a false success that leaves the race unrecorded.
 * (1) setxattr is called EXACTLY ONCE (--dry-run resolves the count and stops there);
 * 0 calls would report success changing nothing, >1 would disarm unrelated code.
 * (2) Stubs are 16-byte spaced and shaped as above (lld without BTI/PAC); with
 * -mbranch-protection=bti they are 32 bytes and C stops being constant, caught on the
 * second pair.
 *
 * The tool prints nothing, at any verbosity: an answer is its exit code, and that is
 * what every caller reads (module/post-fs-data.sh, module/service.sh, status.sh).
 * 0 patch present; 1 vold absent or --check saw an intact stub; 2 could not parse the
 * ELF, find the symbol or the stub; 3 could not write. --file answers --check/--dry-run
 * as on a live process and is never modified.
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "vold-common.h"   /* the ELF-reading half both vold patchers share */

#define TARGET_SYM "setxattr"

/* Bytes written into the stub: mov w0, #0 ; ret */
#define PATCH_MOV0  0x52800000u
#define PATCH_RET   0xd65f03c0u
static const uint32_t PATCH_WORDS[2] = { PATCH_MOV0, PATCH_RET };

#define EXIT_OK         0
#define EXIT_NO_VOLD    1
#define EXIT_NO_RESOLVE 2
#define EXIT_NO_WRITE   3

/* Already-patched stub, in either form this project writes into vold:
 *
 *   ours:      mov w0,#0 ; ret / add / br                — `ldr` is gone,
 *              recognised by the surviving `add` and `br`;
 *   redirected: ldr x17,#8 ; br x17 ; .quad <handler>     — what vold-fusefs.c
 *              writes over the mount and umount2 trampolines.
 *
 * The second form has to be recognised here, or the count lies. A trampoline
 * carrying it has no canonical `ldr` left, so decode_stub() rejects it and it
 * is neither a stub nor "patched" — it becomes a JUMP_SLOT "without a
 * trampoline". With both tools installed on the device that made --selftest
 * report "сопоставление НЕПОЛНОЕ": 3 without a stub against 1 patched. The 3
 * are setxattr (ours) plus mount and umount2 (redirected). Measured 2026-10-08,
 * same vold both ways: live -> 470 matched / 3 missing / 1 patched; the file
 * /system/bin/vold, where no patch is installed -> 473 / 0 / 0.
 *
 * Note this is a diagnostic-only widening: it feeds collect_stubs()' patched
 * list, which resolve() consults for the TARGET symbol's own stub address, and
 * that address is never one of these two. */
static bool looks_patched(const uint8_t *p) {
    uint32_t w0, w1, w2, w3;
    memcpy(&w0, p + 0, 4);
    memcpy(&w1, p + 4, 4);
    memcpy(&w2, p + 8, 4);
    memcpy(&w3, p + 12, 4);

    /* Redirected to an absolute handler address; the quad at +8 is the handler
     * and is not part of the shape. The two words come from vold-common.h, so
     * this reader cannot drift from the writer in vold-fusefs.c. */
    if (w0 == STUB_PATCH_W0 && w1 == STUB_PATCH_W1) return true;

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

static int resolve(Src *s, Resolved *r) {
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
        free(rel);
        return EXIT_NO_RESOLVE;
    }

    /* 2. All stubs in one pass. */
    Stubs st;
    if (collect_stubs(s, &st) != 0) {
        free(rel);
        return EXIT_NO_RESOLVE;
    }

    /* 3. .plt layout from foreign pairs — works even when the symbol's own ldr is gone. */
    r->plt_delta = plt_layout_delta(st.va, st.target, st.n, rel, nrel);
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
        stubs_free(&st);
        return EXIT_NO_RESOLVE;
    } else {
        int64_t cand = (int64_t)r->plt_delta * 16 +
                       (int64_t)r->reloc_index * 16;
        if (cand <= 0) {
            stubs_free(&st);
            return EXIT_NO_RESOLVE;
        }
        r->stub_va = (uint64_t)cand;
        r->already_patched = stubs_is_patched(&st, r->stub_va);
        if (!r->already_patched) {
            stubs_free(&st);
            return EXIT_NO_RESOLVE;
        }
    }

    r->call_sites = count_call_sites(s, r->stub_va);

    /* The patch's PREMISE, not the layout: it only makes sense because vold calls
     * setxattr once, from SetDefaultAcl. 0 calls would report success changing
     * nothing; >1 would also break unrelated code — both refuse. Works on an
     * already-patched vold: `bl` in .text is untouched, only the stub's `ldr` is gone. */
    if (r->call_sites != 1) {
        stubs_free(&st);
        return EXIT_NO_RESOLVE;
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

    size_t npatched = st.npatched;
    stubs_free(&st);

    bool explained = ((size_t)missing == npatched);
    bool ok = (matched + missing == (int)n) && ambiguous == 0 &&
              badtype == 0 && dupes == 0 && explained;
    return ok ? EXIT_OK : EXIT_NO_RESOLVE;
}

static void usage(void) {
    fputs("usage: vold-noacl [--wait SEC] [--pid PID] [--check] [--dry-run]\n"
          "                  [--file ELF] [--selftest]\n"
          "\n"
          "  --selftest   check that the mapping is one-to-one: every JUMP_SLOT\n"
          "               lands on exactly one stub, and the only JUMP_SLOTs\n"
          "               without a stub are the ones already patched. Works on\n"
          "               a live vold and on --file ELF; writes nothing either\n"
          "               way. It answers a different question from --check,\n"
          "               which reports whether THIS symbol is patched.\n",
          stderr);
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
        } else {
            usage();
            return EXIT_NO_RESOLVE;
        }
    }

    /* The resolution below reads the target's own ELF tables, so it takes the
     * identical path on 11 through 17 — nothing here is release-dependent. */
    Src s;
    memset(&s, 0, sizeof(s));
    s.fd = -1;

    char exe[4096] = {0};

    if (file) {
        s.is_proc = false;
        s.label = file;
        s.fd = open(file, O_RDONLY | O_CLOEXEC);
        if (s.fd < 0) {
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
            rc = resolve(&s, &r);
            if (rc == EXIT_OK) {

                /* The file is never modified, so only a live process is worth writing.
                 * But --check/--dry-run must answer identically here and there, or an
                 * offline check would lie about "no patch". */
                uint8_t cur[16];
                if (src_pread(&s, r.stub_va, cur, sizeof(cur)) == 0) {

                    bool bytes_patched =
                        memcmp(cur, PATCH_WORDS, sizeof(PATCH_WORDS)) == 0;
                    if (r.already_patched || bytes_patched) {
                        if (!bytes_patched) {
                            src_close(&s);
                            return EXIT_NO_RESOLVE;
                        }
                    } else {
                        uint64_t tgt = 0;
                        if (!decode_stub(cur, r.stub_va, &tgt) ||
                            tgt != r.got_slot) {
                            src_close(&s);
                            return EXIT_NO_RESOLVE;
                        }
                        if (check) {
                            src_close(&s);
                            return 1;
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
    } else {
        pid = find_vold(wait_sec, exe, sizeof(exe));
        if (pid < 0) {
            return EXIT_NO_VOLD;
        }
    }

    uint64_t base = 0;
    /* exe is filled by identification (or empty for --pid); it is both filter and output. */
    int lb = find_load_base(pid, exe, &base, exe, sizeof(exe));
    if (lb == -1) {
        return EXIT_NO_RESOLVE;
    }
    if (lb != 0) {
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
        return EXIT_NO_RESOLVE;
    }
    if (src_open_header(&s) != 0) {
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }

    /* --selftest answers the same question for a live vold as for --file: does
     * every JUMP_SLOT land on exactly one stub. It used to be honoured only in
     * the --file branch, so `vold-noacl --selftest` on a device silently ran the
     * normal patch path instead — an option that did nothing and said nothing.
     * Checked here, before the patch is resolved, because the selftest is about
     * the mapping as a whole and is the cheaper question. */
    if (self) {
        int rc = selftest(&s);
        src_close(&s);
        return rc;
    }

    Resolved r;
    int rc = resolve(&s, &r);
    if (rc != EXIT_OK) {
        src_close(&s);
        return rc;
    }

    uint64_t run_addr = base + r.stub_va;

    /* Read the whole stub: 8 bytes suffice to compare, but decode_stub also checks `add` and `br`. */
    uint8_t cur[16];
    if (mem_read(pid, run_addr, cur, sizeof(cur)) != 0) {
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }

    bool bytes_patched = memcmp(cur, PATCH_WORDS, sizeof(PATCH_WORDS)) == 0;
    if (r.already_patched || bytes_patched) {
        if (!bytes_patched) {
            src_close(&s);
            return EXIT_NO_RESOLVE;
        }
        src_close(&s);
        return EXIT_OK;
    }

    /* Before writing, confirm the stub is adrp+ldr+add+br and its ldr reads our GOT slot. */
    uint64_t tgt = 0;
    if (!decode_stub(cur, r.stub_va, &tgt) || tgt != r.got_slot) {
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }

    if (check) {
        src_close(&s);
        return 1;
    }
    if (dry_run) {
        src_close(&s);
        return EXIT_OK;
    }

    if (mem_write(pid, run_addr, PATCH_WORDS, sizeof(PATCH_WORDS)) != 0) {
        src_close(&s);
        return EXIT_NO_WRITE;
    }

    uint8_t back[8];
    if (mem_read(pid, run_addr, back, sizeof(back)) != 0 ||
        memcmp(back, PATCH_WORDS, sizeof(PATCH_WORDS)) != 0) {
        src_close(&s);
        return EXIT_NO_WRITE;
    }


    src_close(&s);
    return EXIT_OK;
}
