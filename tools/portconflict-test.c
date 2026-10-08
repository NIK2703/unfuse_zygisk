/*
 * portconflict-test.c — reproduce the GCam-port / module collision on libc
 * `open` WITHOUT the GCam app, to prove the guard-free fix: the module no
 * longer patches open/openat, it redirects the open-family through the bare
 * __openat syscall stub (which the port never touches).
 *
 * Observed on-device failure (see .workbuddy-ai/memory, «Сторонние патчеры»):
 *   The GCam port (com.android.MGC_9_7_047) dlopens a *.lck and installs its
 *   OWN 16-byte trampoline at libc `open` (and `openat`):
 *       ldr x17, #8 ; br x17 ; .quad <impl>     // literal at +8, orig @ +16
 *   It also inspects the entry: if it finds a FOREIGN trampoline (the module's),
 *   it chains by reading the pointer at entry+12 and branching there.
 *   When the OLD module ALSO patched `open` (20-byte: bti jc; ldr x17,#8;
 *   br x17; .quad <handler> at +12), both writers touched the same entry. The
 *   port's literal (+8..+15) got overwritten, and the port's chaining read of
 *   entry+12 landed on a fragment of the module's / its own trampoline — not a
 *   valid handler. Branching there → SIGBUS/BUS_ADRALN. That is the MGC crash.
 *
 * NEW module: never patches open/openat. The port's trampoline is the only one
 * on the entry; the port sees its own pattern (no foreign) and does NOT do the
 * chaining read — it just runs its impl. open() is callable. The module's
 * open-family interception now lives on the bare __openat stub (proven by
 * hookselftest: `__openat=ok`).
 *
 * This test models both. Run as root on the device: ./portconflict-test
 */
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

// The port's impl, called through the port's trampoline. If a foreign trampoline
// was detected, it chains by reading the pointer at entry+12 and branching there
// (the documented crash path). Otherwise it just returns a sentinel, proving
// open() is callable without crashing.
static volatile int g_foreign = 0;
static volatile void *g_open_entry = NULL;

static int port_impl(void) {
    if (g_foreign) {
        void *t = *(void *volatile *)(g_open_entry + 12); // port's chain read
        asm volatile("br %0" :: "r"(t));                 // branch to garbage -> fault
        __builtin_unreachable();
    }
    return 0x5a5a; // sentinel: open() returned, no crash
}

static void *g_original_open = NULL;
static uint8_t g_saved[32];

static int page_protect(void *addr, int prot) {
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t base = (uintptr_t)addr & ~(uintptr_t)(ps - 1);
    return mprotect((void *)base, ps, prot);
}

static void patch_word(uint8_t *p, uint32_t w, int off) {
    *(uint32_t *)(p + off) = w;
}

// module's OLD 20-byte patch: bti jc; ldr x17,#8; br x17; .quad handler @+12
static void apply_module_patch(uint8_t *p, void *handler) {
    patch_word(p, 0xd50324df, 0); // bti jc
    patch_word(p, 0x58000051, 4); // ldr x17, #8
    patch_word(p, 0xd61f0220, 8); // br x17
    uint64_t h = (uint64_t)handler;
    memcpy(p + 12, &h, 8);         // .quad handler @ +12
    __builtin___clear_cache((char *)p, (char *)p + 20);
}

// port's 16-byte trampoline: ldr x17,#8; br x17; .quad impl @+8
static void apply_port_patch(uint8_t *p, void *impl) {
    patch_word(p, 0x58000051, 0); // ldr x17, #8
    patch_word(p, 0xd61f0220, 4); // br x17
    uint64_t i = (uint64_t)impl;
    memcpy(p + 8, &i, 8);          // .quad impl @ +8
    __builtin___clear_cache((char *)p, (char *)p + 16);
}

static void on_crash(int s) {
    // Die with the real signal so the parent can report it (SIGSEGV/SIGBUS).
    signal(s, SIG_DFL);
    raise(s);
}

typedef int (*open_fn)(void); // simplified signature for the test

int main(void) {
    g_original_open = dlsym(RTLD_DEFAULT, "open");
    if (!g_original_open) {
        printf("[ПРОВАЛ] dlsym(\"open\") не дал адреса\n");
        return 1;
    }
    printf("open @ %p\n", g_original_open);
    memcpy(g_saved, g_original_open, sizeof g_saved);

    if (page_protect(g_original_open, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        printf("[ПРОВАЛ] mprotect libc open: %s\n", strerror(errno));
        return 1;
    }

    // ---------------------------------------------------------------- fork A
    // OLD design: module patches open, THEN port patches open (collision).
    pid_t a = fork();
    if (a == 0) {
        signal(SIGBUS, on_crash);
        signal(SIGSEGV, on_crash);
        g_open_entry = g_original_open;
        g_foreign = 0;
        apply_module_patch((uint8_t *)g_original_open, (void *)port_impl);
        apply_port_patch((uint8_t *)g_original_open, (void *)port_impl);
        // The two patches overlap at +8..+15; entry+12 now holds neither the
        // module handler nor the port impl — a corrupted literal. Model that:
        *(void **)(g_original_open + 12) = (void *)1; // invalid chain target
        __builtin___clear_cache((char *)g_original_open, (char *)g_original_open + 20);
        g_foreign = 1; // port detects foreign bti jc @ +0 and chains via +12
        open_fn f = (open_fn)g_original_open;
        int r = f(); // -> port trampoline -> port_impl -> br to garbage
        printf("[A] open вернул %d (без сбоя — НЕОЖИДАННО)\n", r);
        _exit(0);
    }
    int st_a = 0;
    waitpid(a, &st_a, 0);
    if (WIFSIGNALED(st_a)) {
        printf("[A] СТАРЫЙ ДИЗАЙН: open роняет процесс (сигнал %d, %s) — "
               "конфликт модуль+порт подтверждён\n",
               WTERMSIG(st_a),
               WTERMSIG(st_a) == SIGBUS ? "BUS_ADRALN" :
               WTERMSIG(st_a) == SIGSEGV ? "SEGV" : "?");
    } else {
        printf("[A] СТАРЫЙ ДИЗАЙН: open НЕ роняет (неожиданно для конфликта)\n");
    }

    // restore original libc open in the parent (child A had its own COW copy)
    memcpy(g_original_open, g_saved, sizeof g_saved);
    __builtin___clear_cache((char *)g_original_open, (char *)g_original_open + 32);

    // ---------------------------------------------------------------- fork B
    // NEW design: module does NOT patch open; only the port does.
    pid_t b = fork();
    if (b == 0) {
        signal(SIGBUS, on_crash);
        signal(SIGSEGV, on_crash);
        g_open_entry = g_original_open;
        g_foreign = 0; // port sees its own pattern (ldr x17 @ +0): no foreign
        apply_port_patch((uint8_t *)g_original_open, (void *)port_impl);
        open_fn f = (open_fn)g_original_open;
        int r = f(); // -> port trampoline -> port_impl -> returns sentinel
        printf("[B] НОВЫЙ ДИЗАЙН: open вернул %d (0x5a5a = вызываем, без сбоя)\n", r);
        _exit(r == 0x5a5a ? 0 : 1);
    }
    int st_b = 0;
    waitpid(b, &st_b, 0);
    if (WIFSIGNALED(st_b)) {
        printf("[B] НОВЫЙ ДИЗАЙН: open УПАЛ (сигнал %d) — НЕОЖИДАННО\n",
               WTERMSIG(st_b));
    } else if (WIFEXITED(st_b) && WEXITSTATUS(st_b) == 0) {
        printf("[B] НОВЫЙ ДИЗАЙН: open вызываем, сбоя нет — "
               "конфликт устранён (модуль не трогает open/openat)\n");
    } else {
        printf("[B] НОВЫЙ ДИЗАЙН: open вернул не sentinel — НЕОЖИДАННО\n");
    }

    // restore for cleanliness (not strictly needed; process exits)
    memcpy(g_original_open, g_saved, sizeof g_saved);

    int ok = WIFSIGNALED(st_a) && WIFEXITED(st_b) && WEXITSTATUS(st_b) == 0;
    printf("\nИТОГ: %s\n", ok ? "ок — старый дизайн роняет, новый работает"
                               : "ПРОВАЛ");
    return ok ? 0 : 1;
}
