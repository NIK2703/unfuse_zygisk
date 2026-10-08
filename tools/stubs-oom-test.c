/*
 * stubs-oom-test.c — does a failed growth in Stubs leave the structure safe to
 *                    free?
 *
 * ============================== why this is a host test
 *
 * The branch under test is the one that runs when realloc() fails. Nothing on a
 * device makes a 4 KB realloc fail on demand, and the allocation is 4 KB — so
 * the only way to reach the branch is to supply the allocator. This file
 * includes tools/stubs.h (VOLD_STUBS_SRC) with malloc/calloc/realloc/free
 * renamed by macro, so the probe replaces the allocator *for the real code*, not
 * for a copy of it. A hand-written replica of stubs_add() would prove something
 * about the replica.
 *
 * That include is the reason Stubs lives in its own header: including
 * vold-noacl.c instead would drag in <elf.h> (absent from MSYS2), O_CLOEXEC and
 * pread/pwrite (absent from MinGW) — three host gaps that have nothing to do
 * with the code under test.
 *
 * ============================== what is being proven
 *
 * stubs_add() grows two parallel arrays. realloc() frees the old block as soon
 * as it returns non-NULL, so the sequence
 *
 *     t = realloc(st->target, cap);
 *     v = realloc(st->va,     cap);
 *     if (!t || !v) { free(t); free(v); return -1; }
 *     st->target = t;
 *     st->va     = v;
 *
 * has two separate defects once the SECOND realloc fails: st->target is never
 * updated (so it still points at the block realloc just freed), and the
 * temporary t — the only live pointer to the new block — is freed. The caller
 * (collect_stubs) then answers -1 with stubs_free(), which frees st->target
 * again. A double free, and in a tool that goes on to write `mov w0,#0; ret`
 * into vold's .plt a corrupted heap can move that write.
 *
 * The first realloc failing is *not* a defect: st->target is untouched and
 * still live. Both cases are asserted here, so the test says precisely which
 * failure it is about instead of "realloc failure is bad".
 *
 * ============================== how the A/B is run
 *
 * tools/test-stubs-oom.sh builds this file twice: once against the real
 * tools/stubs.h and once against a copy with the old body substituted back in.
 * The old build must report the double free; the new one must not. The
 * substitution is asserted to match exactly once, so the "old" build cannot
 * quietly become a copy of the new one — which would make the A/B vacuous.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ the allocator
 *
 * Every block handed out is recorded, and free() is answered from the record
 * instead of from libc: a double free is *counted* and then dropped, because
 * handing it to libc would abort the probe before it could report anything. */

enum { MAXBLK = 4096 };
static struct { void *p; int freed; } g_blk[MAXBLK];
static int  g_nblk;
static int  g_double_free;    /* free() of a block that was already freed   */
static int  g_free_unknown;   /* free() of a pointer we never handed out    */
static long g_realloc_calls;
static long g_fail_realloc = -1;   /* fail the Nth realloc call, 1-based */

static void blk_add(void *p) {
    if (!p) return;
    if (g_nblk < MAXBLK) { g_blk[g_nblk].p = p; g_blk[g_nblk].freed = 0; g_nblk++; }
}

static int blk_find(void *p) {
    for (int i = 0; i < g_nblk; i++) if (g_blk[i].p == p) return i;
    return -1;
}

static void blk_reset(void) {
    g_nblk = 0;
    g_double_free = 0;
    g_free_unknown = 0;
    g_realloc_calls = 0;
    g_fail_realloc = -1;
}

void *oom_malloc(size_t n)            { void *p = malloc(n); blk_add(p); return p; }
void *oom_calloc(size_t n, size_t sz) { void *p = calloc(n, sz); blk_add(p); return p; }

void *oom_realloc(void *p, size_t n) {
    g_realloc_calls++;
    /* A failing realloc leaves the old block ALONE — that is what makes the
     * half-published state reachable. */
    if (g_realloc_calls == g_fail_realloc) return NULL;
    /* Keep the old pointer as an integer: realloc() has freed what p points at
     * by the time it returns, so the address is only compared, never followed. */
    uintptr_t old = (uintptr_t)p;
    void *q = realloc(p, n);
    if (q && old) { int i = blk_find((void *)old); if (i >= 0) g_blk[i].freed = 1; }
    blk_add(q);
    return q;
}

void oom_free(void *p) {
    if (!p) return;
    int i = blk_find(p);
    if (i < 0) { g_free_unknown++; return; }
    if (g_blk[i].freed) { g_double_free++; return; }
    g_blk[i].freed = 1;
    free(p);
}

void *oom_malloc(size_t n);
void *oom_calloc(size_t n, size_t sz);
void *oom_realloc(void *p, size_t n);
void  oom_free(void *p);

#ifndef VOLD_STUBS_SRC
#define VOLD_STUBS_SRC "stubs.h"
#endif

#define malloc  oom_malloc
#define calloc  oom_calloc
#define realloc oom_realloc
#define free    oom_free
#include VOLD_STUBS_SRC
#undef malloc
#undef calloc
#undef realloc
#undef free

/* ------------------------------------------------------------------ the cases */

static int g_fail;
static int g_case3_ran;              /* case 3 reached its stubs_free()      */
static int g_case3_double_free;      /* ...and the free was a double free    */

static void fail(const char *what) {
    printf("  ПРОВАЛ: %s\n", what);
    g_fail = 1;
}

/* Control: the ordinary path must still grow and index correctly. Without this
 * a "fix" that refuses every growth would pass the two cases below. */
static void case_growth_ok(void) {
    printf("случай 1: рост без отказа (контроль)\n");
    blk_reset();
    Stubs st;
    memset(&st, 0, sizeof st);

    for (size_t i = 0; i < 600; i++) {
        if (stubs_add(&st, 1000 + i, 2000 + i) != 0) { fail("stubs_add отказал"); return; }
    }
    if (st.n != 600)    { fail("n != 600"); return; }
    if (st.cap != 1024) { fail("cap != 1024 после двух ростов"); return; }
    for (size_t i = 0; i < 600; i++) {
        int hits = 0;
        uint64_t va = stubs_lookup(&st, 1000 + i, &hits);
        if (hits != 1 || va != 2000 + i) { fail("stubs_lookup вернул не ту пару"); return; }
    }
    stubs_free(&st);
    if (g_double_free || g_free_unknown) { fail("освобождение небезопасно"); return; }
    printf("  ок: 600 записей, cap=1024, каждая пара находится\n");
}

/* The failure the old body got right: nothing was published yet. Asserted so
 * the test cannot be read as "any realloc failure is unsafe". */
static void case_first_realloc_fails(void) {
    printf("случай 2: отказывает ПЕРВЫЙ realloc роста\n");
    blk_reset();
    Stubs st;
    memset(&st, 0, sizeof st);

    g_fail_realloc = 1;
    if (stubs_add(&st, 1, 2) == 0) { fail("stubs_add вернул успех при отказе realloc"); return; }
    /* One call in the fixed body, which returns before the second; two in the
     * old one, which evaluates both reallocs before testing either. Either way
     * the FIRST call is the one that failed, which is what this case is about —
     * so the count is bounded, not pinned. */
    if (g_realloc_calls < 1 || g_realloc_calls > 2) { fail("вызовов realloc не 1 и не 2"); return; }

    stubs_free(&st);                       /* exactly what collect_stubs() does */
    if (g_double_free)  { fail("двойное освобождение после отказа первого realloc"); return; }
    if (g_free_unknown) { fail("free() указателя, которого мы не выдавали"); return; }
    printf("  ок: цель роста не тронута, освобождение безопасно\n");
}

/* The failure the old body got wrong: the first realloc has already freed the
 * old target block, and the second one fails.
 *
 * The counter is reset before the growth, so the two reallocs it makes are calls
 * 1 and 2 — hence g_fail_realloc = 2. Getting that wrong is silent: the
 * injection does not fire, stubs_add() succeeds, and the *assertion* about the
 * return value is what fails, not the double free. That is why the verdict of
 * this case is also published as a marker line (main) for the script to match —
 * a run whose injection missed must not be able to look like a pass. */
static void case_second_realloc_fails(void) {
    printf("случай 3: отказывает ВТОРОЙ realloc роста\n");
    blk_reset();
    Stubs st;
    memset(&st, 0, sizeof st);

    for (size_t i = 0; i < 512; i++) {
        if (stubs_add(&st, 1000 + i, 2000 + i) != 0) { fail("рост на первых 512 не удался"); return; }
    }
    if (st.cap != 512) { fail("ожидалось cap=512 после первого роста"); return; }

    g_realloc_calls = 0;
    g_fail_realloc = 2;              /* #1 = target (succeeds), #2 = va (fails) */
    if (stubs_add(&st, 9999, 9999) == 0) { fail("stubs_add вернул успех при отказе realloc"); return; }
    /* Pins the injection to the second realloc of this growth — if the growth
     * order ever changes, this fails instead of the probe quietly testing the
     * case it already covers. */
    if (g_realloc_calls != 2) { fail("до отказа дошло не 2 вызова realloc"); return; }

    stubs_free(&st);                 /* exactly what collect_stubs() does */
    g_case3_double_free = g_double_free;
    g_case3_ran = 1;
    if (g_double_free) {
        fail("двойное освобождение: st->target остался на блоке, освобождённом realloc");
        return;
    }
    if (g_free_unknown) { fail("free() указателя, которого мы не выдавали"); return; }
    printf("  ок: st->target обновлён только после успеха, освобождение безопасно\n");
}

int main(void) {
    printf("=== stubs-oom: рост Stubs при отказе realloc ===\n");
    case_growth_ok();
    case_first_realloc_fails();
    case_second_realloc_fails();

    /* The verdict the script matches on, in a form that cannot be reached
     * without the injection having fired: "не-дошло" means case 3 never got to
     * its free(), so the run proves nothing and is reported as a failure. */
    printf("МЕТКА: %s\n",
           !g_case3_ran     ? "не-дошло" :
            g_case3_double_free ? "двойное-освобождение" : "освобождение-чисто");

    printf("%s\n", g_fail ? "ИТОГ: ПРОВАЛ" : "ИТОГ: ок");
    return g_fail ? 1 : 0;
}
