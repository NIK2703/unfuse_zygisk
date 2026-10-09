/*
 * test-arm32-patch.c — печатает байты патча входа ARM32 из src/arm32_patch.h.
 *
 * Сам по себе он ничего не утверждает: строки разбирает и сверяет
 * tools/test-arm32-patch.sh — против arm_patch_bytes/arm_patch_size из
 * tools/verify-hook-targets.py, то есть против той же формы, что проверена на
 * всех восьми arm32-образах libc. Смысл именно в сверке с verifier'ом, а не с
 * копией констант: если модуль и verifier разойдутся в кодировании, патч на
 * устройстве встанет не так, как на образе проверено.
 *
 * Формат строки: <raw-адрес> <handler> <ширина> <hex-байты в порядке памяти>.
 */
#include <stdint.h>
#include <stdio.h>

#include "arm32_patch.h"

static void dump(uintptr_t raw, uint32_t handler) {
    uint8_t b[ARM32_PATCH_THUMB2];
    const size_t n = arm32_patch_bytes(raw, handler, b);
    printf("%08lx %08x %zu ", (unsigned long)raw, (unsigned)handler, n);
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

int main(void) {
    /* Адреса — с реальных целей эталонов плюс синтетические границы
     * выравнивания: 4-выровненный Thumb, 2-выровненный Thumb, ARM. */
    static const uintptr_t addrs[] = {
        0x1001u,     /* Thumb, 4-выровнен (addr 0x1000) */
        0x2003u,     /* Thumb, 2-выровнен (addr 0x2002) — форма 10 байт */
        0x3000u,     /* ARM, 4-выровнен */
        0x0005a2a1u, /* __open_2 (Thumb, 4-выровнен) */
        0x00060fbdu, /* renameat (Thumb, 4-выровнен) */
        0x000592efu, /* link (Thumb, 2-выровнен) */
        0x000a1980u, /* mkdirat (ARM) */
        0x000a1800u, /* __openat (ARM) */
    };
    static const uint32_t handlers[] = {
        0x00000000u, 0x00000001u, 0x00000002u,
        0xdeadbeefu, 0x12345678u, 0xffffffffu,
    };

    for (size_t i = 0; i < sizeof addrs / sizeof addrs[0]; i++)
        for (size_t j = 0; j < sizeof handlers / sizeof handlers[0]; j++)
            dump(addrs[i], handlers[j]);
    return 0;
}
