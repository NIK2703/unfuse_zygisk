/*
 * arm32_patch.h — формы патча входа ARM32 (armeabi-v7a), чистыми функциями.
 *
 * Вынесено из hook_libc.cpp по тому же правилу, что proc-name.h, stubs.h и
 * openat_stub.h: (адрес, обработчик) → байты, без устройства, без ELF, без
 * заголовков Android. Так форма проверяется на хосте
 * (tools/test-arm32-patch.sh), а не «на глаз» по дизассемблеру.
 *
 * Значения — РОВНО те, что у arm_patch_size/arm_patch_bytes в
 * tools/verify-hook-targets.py, а тот проверен на всех восьми arm32-образах
 * libc (11..17). Совпадение байт доказывает test-arm32-patch.sh: он сравнивает
 * вывод этих функций с выводом питоновского verifier'а, а не с копией
 * констант. Одна форма на все релизы — ветвлений по версии Android нет.
 *
 * Формы (бит 0 адреса — режим: у Thumb выставлен, у ARM-функции снят):
 *
 *   Thumb-2, entry%4==0 — 8 байт:
 *     ldr.w pc, [pc, #0]   ; полуслова F8DF F000 → как LE-слово 0xF000F8DF
 *     .word <handler>      ; литерал по entry+4; PC = Align(entry,4)+4 = entry+4
 *   Thumb-2, entry%4==2 — 10 байт (литерал по entry+4 не выровнен по слову):
 *     movw r12, #lo
 *     movt r12, #hi
 *     bx   r12
 *   ARM, всегда 4-выровнен — 8 байт:
 *     ldr  pc, [pc, #-4]   ; 0xE51FF004: PC = entry+8, [PC-4] = entry+4
 *     .word <handler>
 *
 * Ни BTI, ни PAC на AArch32 нет, поэтому паддинга-площадки в начале нет вовсе
 * (в отличие от AArch64, где bti jc обязателен, если образ объявит свойство BTI).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* ldr.w pc, [pc, #0]: полуслова F8DF F000, то есть как LE-слово 0xF000F8DF.
 * Обратный порядок (0xF8DFF000) дизассемблируется как `bl` — молча не тот патч. */
#define ARM32_THUMB_LDR_PC 0xf000f8dfu
/* ldr pc, [pc, #-4]: PC = addr+8, [PC-4] = addr+4 — литерал сразу за инструкцией. */
#define ARM32_ARM_LDR_PC 0xe51ff004u
#define ARM32_THUMB_BX_R12 0x4760u

#define ARM32_PATCH_ARM 8u     /* ARM-режим; и Thumb-2 при entry%4==0 */
#define ARM32_PATCH_THUMB2 10u /* Thumb-2 при entry%4==2: литерал не выровнен */

/* Бит 0 адреса — режим (EABI). */
static inline int arm32_is_thumb(uintptr_t addr) { return (addr & 1u) != 0u; }

/* Сколько байт нужно под патч по этому (сырому, с битом режима) адресу. */
static inline size_t arm32_patch_width(uintptr_t addr) {
    if (arm32_is_thumb(addr) && ((addr & ~(uintptr_t)1u) % 4u) != 0u) {
        return ARM32_PATCH_THUMB2;
    }
    return ARM32_PATCH_ARM;
}

/* Полуслова Thumb-2 MOVW/MOVT (T3/T1) для rd=12 — в out[0], out[1]. */
static inline void arm32_thumb_mov_imm16(uint16_t imm16, int movt, uint16_t out[2]) {
    const uint16_t i = (uint16_t)((imm16 >> 11) & 1u);
    const uint16_t imm4 = (uint16_t)((imm16 >> 12) & 0xFu);
    const uint16_t imm3 = (uint16_t)((imm16 >> 8) & 7u);
    const uint16_t imm8 = (uint16_t)(imm16 & 0xFFu);
    out[0] = (uint16_t)((movt ? 0xF2C0u : 0xF240u) | (i << 10) | imm4);
    out[1] = (uint16_t)((imm3 << 12) | (12u << 8) | imm8);
}

/*
 * Байты патча в порядке СЛЕДОВАНИЯ В ПАМЯТИ (little-endian), как их кладёт
 * patch_entry. out вмещает не меньше arm32_patch_width(addr) байт; возвращается
 * записанная ширина. addr — сырой адрес цели (с битом режима).
 */
static inline size_t arm32_patch_bytes(uintptr_t addr, uint32_t handler, uint8_t *out) {
    if (arm32_is_thumb(addr)) {
        const uintptr_t even = addr & ~(uintptr_t)1u;
        if ((even % 4u) == 0u) {
            out[0] = (uint8_t)(ARM32_THUMB_LDR_PC & 0xFFu);
            out[1] = (uint8_t)((ARM32_THUMB_LDR_PC >> 8) & 0xFFu);
            out[2] = (uint8_t)((ARM32_THUMB_LDR_PC >> 16) & 0xFFu);
            out[3] = (uint8_t)((ARM32_THUMB_LDR_PC >> 24) & 0xFFu);
            out[4] = (uint8_t)(handler & 0xFFu);
            out[5] = (uint8_t)((handler >> 8) & 0xFFu);
            out[6] = (uint8_t)((handler >> 16) & 0xFFu);
            out[7] = (uint8_t)((handler >> 24) & 0xFFu);
            return ARM32_PATCH_ARM;
        }
        uint16_t hw[5];
        arm32_thumb_mov_imm16((uint16_t)(handler & 0xFFFFu), 0, hw + 0);
        arm32_thumb_mov_imm16((uint16_t)((handler >> 16) & 0xFFFFu), 1, hw + 2);
        hw[4] = ARM32_THUMB_BX_R12;
        for (int i = 0; i < 5; i++) {
            out[2 * i] = (uint8_t)(hw[i] & 0xFFu);
            out[2 * i + 1] = (uint8_t)(hw[i] >> 8);
        }
        return ARM32_PATCH_THUMB2;
    }

    out[0] = (uint8_t)(ARM32_ARM_LDR_PC & 0xFFu);
    out[1] = (uint8_t)((ARM32_ARM_LDR_PC >> 8) & 0xFFu);
    out[2] = (uint8_t)((ARM32_ARM_LDR_PC >> 16) & 0xFFu);
    out[3] = (uint8_t)((ARM32_ARM_LDR_PC >> 24) & 0xFFu);
    out[4] = (uint8_t)(handler & 0xFFu);
    out[5] = (uint8_t)((handler >> 8) & 0xFFu);
    out[6] = (uint8_t)((handler >> 16) & 0xFFu);
    out[7] = (uint8_t)((handler >> 24) & 0xFFu);
    return ARM32_PATCH_ARM;
}
