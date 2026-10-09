/*
 * openat_stub.h — поиск стаба сисколла openat по форме, а не по имени.
 *
 * ============================== зачем это вообще нужно
 *
 * Модуль вешает свои переходники на входы libc. Порт GCam (com.android.MGC_*)
 * грузит свои codec_*.lck и вешает на те же входы СВОИ. Найдя на входе чужой
 * переходник, он строит трамплин, который читает указатель по entry+12, — а
 * перед этим сам затирает первые 16 байт. По entry+12 оказывается мусор (его
 * слово + остаток нашего литерала) → br x17 по невыровненному адресу →
 * SIGBUS/BUS_ADRALN. Разбор — memory/2026-10-08.md.
 *
 * Формой патча это не лечится: подстановка в живой процесс любой формы (порта,
 * модуля, без bti jc, литерал по +16) роняет его одинаково. Сторожевой поток
 * тоже не ответ — сторожа в этом проекте запрещены (memory/MEMORY.md,
 * «Запреты»). Остаётся единственный выход: не ставить свой переходник на те
 * входы, которые патчит порт, то есть на open и openat.
 *
 * ============================== почему именно стаб сисколла
 *
 * Всё open-семейство сходится в одну функцию. На эталонном образе 16
 * (device/libc/libc-arm64.so):
 *
 *   open64/open     0x84f74 → bl __openat   (после фильтров HyperOS
 *                                            custom_rom_hide_*)
 *   openat64/openat 0x85478 → bl __openat
 *   __open_2        0x85240 → bl __openat   (и передаёт режим = 0!)
 *   __openat_2      0x85708 → bl __openat
 *
 * а сам __openat — голый стаб сисколла:
 *
 *   mov x8, #0x38 (__NR_openat)   d2800708
 *   svc  #0                       d4000001
 *   …проверка errno, у каждого релиза своя…
 *   ret                           d65f03c0
 *
 * Порт его не трогает — он патчит только входы open и openat, — поэтому один
 * переходник здесь заменяет четыре снятых входа и не сталкивается ни с чем.
 *
 * __open_2 и __openat_2 при этом ОСТАЮТСЯ входовыми целями: они зовут __openat
 * с режимом 0, а модуль подставляет туда 0666 (нулевой режим обнулил бы
 * ACL-маску). Это поведение терять нельзя, поэтому эти два входа не снимаем.
 *
 * ============================== почему по форме, а не по имени
 *
 * __openat — ЛОКАЛЬНЫЙ символ, в .dynsym его нет, и dlsym его не видит.
 * Уникальность проверена по всем 16 эталонным образам (8 arm64 + 8 arm32):
 * форма встречается в .text каждого ровно один раз. Это подтверждает
 * tools/test-openat-stub.sh на хосте, и он же сверяет найденный адрес с
 * символом __openat из .symtab — то есть «нашли по форме» доказано, а не
 * предположено.
 *
 * Проверку errno релизы пишут по-разному: 11–16 через cmn x0,#0x1,lsl#12 /
 * cneg x0,x0,hi / b.hi, а 17 — коротким cmn x0,#0xfff / b.hs без cneg. Поэтому
 * шаблон держим по существу (mov x8,#0x38 + svc #0 + ret рядом), а не по
 * конкретной последовательности проверки.
 *
 * ============================== AArch32: та же задача, другая форма
 *
 * На arm32 стаб сисколла тоже один, но выглядит иначе (ARM-режим, 32 байта;
 * одинаково на 11, 16 и 17 — проверено на всех восьми образах):
 *
 *   mov  r12, r7         e1a0c007   ; r7 — номер сисколла, но в Thumb он же
 *   movw r7, #0x142      e3007142   ;   frame pointer, поэтому его берегут
 *   svc  #0              ef000000   ; __NR_openat = 0x142
 *   mov  r7, r12         e1a0700c
 *   cmn  r0, #4096       e3700a01
 *   bxls lr              912fff1e
 *   rsb  r0, r0, #0      e2600000
 *   b    __set_errno_internal       ; хвост — безусловный B (ARM)
 *
 * Пролог `mov r12, r7` здесь и есть признак: без него `movw r7, #0x142`
 * встречается где угодно. Уникальность формы из трёх слов проверена по всем
 * восьми arm32-образам — везде ровно одно вхождение.
 *
 * Хвост — `b`, а не `bx lr`/`ret`. Именно из-за него arm_tail_target в
 * tools/verify-hook-targets.py НЕ считает хвостовой ARM-`b` переходником:
 * иначе все четыре сисколл-стаба приняли бы за переходники и образ остался бы
 * без корней. Здесь по той же причине конец стаба ищется явно, а не через
 * общее правило «коротка и кончается переходом».
 *
 * ============================== почему это отдельный файл
 *
 * Функции чистые: (байты .text, длина) → смещение. Ни адресов, ни разбора ELF,
 * ни устройства — поэтому они проверяются на хосте по эталонным образам, как
 * proc-name.h и stubs.h. В hook_libc.cpp они получают уже разобранный
 * исполняемый сегмент libc. Тот же вынос и по той же причине.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* ============================== AArch64 ============================== */

/* Форма стаба: mov x8,#__NR_openat; svc #0; …; ret. */
#define OPENAT_STUB_MOV_X8_38 0xd2800708u
#define OPENAT_STUB_SVC_0 0xd4000001u
#define OPENAT_STUB_RET 0xd65f03c0u

/* Стаб короче ширины патча патчить нельзя. */
#define OPENAT_STUB_MIN_SIZE 20u

/*
 * Смещение стаба в байтах от начала text, или -1.
 *
 * -1 означает «не найден ИЛИ найден не один раз»: при двух стабах выбор был бы
 * угадыванием, а угадывание здесь — это патч не туда. Отказ, а не догадка.
 *
 * При успехе *out_size получает длину стаба (до ret включительно, кратно 4);
 * она же — размер функции, потому что стаб кончается ret. Так размер берётся
 * без .dynsym, которого у локального символа всё равно нет.
 *
 * text должен быть выровнен на 4: это либо начало сегмента, либо его
 * выровненное смещение.
 */
static inline long openat_stub_find_a64(const void *text, size_t len,
                                        unsigned *out_size) {
    if (text == NULL || len < OPENAT_STUB_MIN_SIZE) return -1;

    const uint32_t *w = (const uint32_t *)text;
    const size_t n = len / 4;
    long found = -1;
    unsigned size = 0;

    for (size_t i = 0; i + 2 < n; i++) {
        if (w[i] != OPENAT_STUB_MOV_X8_38 || w[i + 1] != OPENAT_STUB_SVC_0) continue;
        /* ret должен быть рядом: это обёртка сисколла, а не кусок кода, где
         * mov x8,#0x38 случайно оказался перед svc. 2..5 слов — обе известные
         * формы (ret на +0x14 и на +0x10). */
        for (size_t k = 2; k <= 5 && i + k < n; k++) {
            if (w[i + k] != OPENAT_STUB_RET) continue;
            if (found >= 0) return -1; /* второй стаб — отказ, а не выбор */
            found = (long)(i * 4u);
            size = (unsigned)((k + 1u) * 4u);
            break;
        }
    }

    if (found < 0 || size < OPENAT_STUB_MIN_SIZE) return -1;
    if (out_size != NULL) *out_size = size;
    return found;
}

/* ============================== AArch32 (armeabi-v7a) ============================== */

/* Форма стаба: mov r12,r7; movw r7,#__NR_openat; svc #0; …; b __set_errno_internal. */
#define OPENAT_STUB_ARM_MOV_R12_R7 0xe1a0c007u
#define OPENAT_STUB_ARM_MOVW_R7_142 0xe3007142u
#define OPENAT_STUB_ARM_SVC_0 0xef000000u

/* Хвост стаба — безусловный B (ARM, cond=AL): старший байт 0xea. */
#define OPENAT_STUB_ARM_B_MASK 0xff000000u
#define OPENAT_STUB_ARM_B 0xea000000u

/* Ширина патча входа ARM32 — 8 байт; стаб (32) её перекрывает с запасом. */
#define OPENAT_STUB_ARM_MIN_SIZE 8u

/*
 * Смещение arm32-стаба от начала text, или -1 (не найден или найден не один
 * раз — та же логика отказа, что и у AArch64).
 *
 * Длина считается до хвостового `b` включительно. Между svc и ним у bionic
 * стоят mov r7,r12 / cmn / bxls lr / rsb — четыре слова, поэтому окно 2..8
 * (аналог окна до `ret` у AArch64). Так размер берётся без .symtab, которого у
 * локального символа всё равно нет.
 *
 * text должен быть выровнен на 4: стаб ARM-режима, инструкции 4-байтные.
 */
static inline long openat_stub_find_arm(const void *text, size_t len,
                                        unsigned *out_size) {
    if (text == NULL || len < OPENAT_STUB_ARM_MIN_SIZE) return -1;

    const uint32_t *w = (const uint32_t *)text;
    const size_t n = len / 4;
    long found = -1;
    unsigned size = 0;

    for (size_t i = 0; i + 3 < n; i++) {
        if (w[i] != OPENAT_STUB_ARM_MOV_R12_R7 ||
            w[i + 1] != OPENAT_STUB_ARM_MOVW_R7_142 ||
            w[i + 2] != OPENAT_STUB_ARM_SVC_0) {
            continue;
        }
        /* Конец стаба — первый безусловный B в пределах окна. Нет его — это не
         * обёртка сисколла (та обязана увести ошибку в __set_errno_internal),
         * поэтому пропуск, а не догадка о длине. */
        for (size_t k = 3; k <= 8 && i + k < n; k++) {
            if ((w[i + k] & OPENAT_STUB_ARM_B_MASK) != OPENAT_STUB_ARM_B) continue;
            if (found >= 0) return -1; /* второй стаб — отказ, а не выбор */
            found = (long)(i * 4u);
            size = (unsigned)((k + 1u) * 4u);
            break;
        }
    }

    if (found < 0 || size < OPENAT_STUB_ARM_MIN_SIZE) return -1;
    if (out_size != NULL) *out_size = size;
    return found;
}

/* ============================== выбор по ABI сборки ============================== */

/*
 * Модуль собирается под каждый ABI отдельно, поэтому здесь достаточно
 * препроцессора. На хосте (x86_64) не определён ни один — тогда отказ, и это
 * честно: хост-тест зовёт openat_stub_find_a64 / _arm явно, по классу образа.
 */
static inline long openat_stub_find(const void *text, size_t len,
                                    unsigned *out_size) {
#if defined(__aarch64__)
    return openat_stub_find_a64(text, len, out_size);
#elif defined(__arm__)
    return openat_stub_find_arm(text, len, out_size);
#else
    (void)text;
    (void)len;
    if (out_size != NULL) *out_size = 0;
    return -1;
#endif
}
