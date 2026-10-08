/*
 * stubs.h — the two parallel arrays vold-noacl collects trampolines into.
 *
 * ============================== why this is its own file
 *
 * It is not "the ELF half" and it is not "the vold half": it is the container,
 * and it is the one part of vold-noacl that can be exercised without a vold, an
 * ELF file, or a device. Everything else here either parses ELF (vold-common.h)
 * or decides where to write (vold-noacl.c), so neither can be driven without a
 * target — which is exactly how a latent defect in the growth path survived:
 * there was no way to reach it.
 *
 * tools/stubs-oom-test.c includes THIS header, with malloc/calloc/realloc/free
 * renamed by macro so the probe supplies the allocator. That only works because
 * this file needs nothing but <stdint.h>/<stdlib.h>/<string.h>: including
 * vold-noacl.c instead would drag in <elf.h> (absent on MSYS2), O_CLOEXEC and
 * pread/pwrite (absent on MinGW) — three host gaps with nothing to do with the
 * code under test.
 *
 * ============================== the invariant
 *
 * `target` and `va` are indexed together: entry i is the stub at `va[i]` whose
 * `ldr` reads from GOT slot `target[i]`. `patched` is a separate list — the
 * trampolines that already carry the patch, so they can no longer be found by
 * their GOT slot and are recognised by shape instead.
 *
 * Both arrays must hold at least `cap` entries whenever `n <= cap`. Growth
 * publishes each pointer only once it is known good; see stubs_add().
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
        /* Two reallocs, and the order they are *published* in is the whole
         * point. realloc has already freed the old block by the time it
         * returns non-NULL, so a caller that stored the new `target` and then
         * failed to get a new `va` would leave st->target pointing at freed
         * memory — and collect_stubs() answers a failure from here with
         * stubs_free(), which frees st->target again. A double free in a tool
         * that then pokes vold's .plt with `mov w0,#0; ret` is not a crash in
         * the tool: a corrupted heap can move the write, and vold.rc carries
         * reboot_on_failure.
         *
         * So each pointer is published only once it is known good. If the
         * second realloc fails, st->cap deliberately keeps its old value: both
         * arrays still hold at least `cap` entries, which is the invariant the
         * indexing below relies on. */
        uint64_t *t = realloc(st->target, cap * sizeof(uint64_t));
        if (!t) return -1;
        st->target = t;
        uint64_t *v = realloc(st->va, cap * sizeof(uint64_t));
        if (!v) return -1;
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
