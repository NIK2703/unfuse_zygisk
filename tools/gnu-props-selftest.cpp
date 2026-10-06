/*
 * gnu-props-selftest.cpp — проверка разбора GNU property note (src/gnu_props.h).
 *
 * Разбор идёт в каждом запуске приложения, внутри postAppSpecialize: ошибка в
 * нём — это не неверная строка в логе, а падение всех приложений сразу. Поэтому
 * здесь проверяются ровно те свойства, на которые разбор опирается:
 *
 *   1. корректная заметка: имя GNU, тип NT_GNU_PROPERTY_TYPE_0, одна или
 *      несколько свойств — слово возможностей читается;
 *   2. чужие заметки (другое имя, другой тип) не считаются;
 *   3. обрезанные данные не читаются за границей сегмента: заявленный размер
 *      больше доступного, и наоборот;
 *   4. враждебные размеры (0xffffffff) не переполняют арифметику;
 *   5. мусор во всех байтах не приводит к чтению за границей;
 *   6. НАСТОЯЩАЯ заметка из объектного файла, собранного с
 *      -mbranch-protection=standard, разбирается как BTI+PAC (пункт 6 требует
 *      пути к дампу секции .note.gnu.property, см. tools/test-gnu-props.sh).
 *
 * Код возврата: 0 — всё сошлось, 1 — есть расхождение.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "gnu_props.h"

namespace {

int g_fail = 0;
int g_pass = 0;

void check(bool ok, const char *what) {
    printf("    [%s] %s\n", ok ? "ок" : "ПРОВАЛ", what);
    if (ok) g_pass++;
    else g_fail++;
}

void put32(std::vector<uint8_t> &v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

// Elf64_Nhdr: имена и описания выравниваются на 4 байта.
void put_note(std::vector<uint8_t> &v, uint32_t namesz, const char *name,
              uint32_t type, const std::vector<uint8_t> &desc) {
    put32(v, namesz);
    put32(v, static_cast<uint32_t>(desc.size()));
    put32(v, type);
    for (uint32_t i = 0; i < namesz; i++) v.push_back(static_cast<uint8_t>(name[i]));
    while (v.size() % 4) v.push_back(0);
    for (uint8_t b : desc) v.push_back(b);
    while (v.size() % 4) v.push_back(0);
}

// Elf64_Prop: тип, размер данных, данные, выравнивание до 8.
void put_prop(std::vector<uint8_t> &desc, uint32_t type, const std::vector<uint8_t> &data) {
    put32(desc, type);
    put32(desc, static_cast<uint32_t>(data.size()));
    for (uint8_t b : data) desc.push_back(b);
    while (desc.size() % 8) desc.push_back(0);
}

std::vector<uint8_t> u32bytes(uint32_t x) {
    std::vector<uint8_t> v;
    put32(v, x);
    return v;
}

UnfuseImageProps parse(const std::vector<uint8_t> &v) {
    UnfuseImageProps p;
    memset(&p, 0, sizeof p);
    // Копия в буфер с запасом: если разбор выйдет за границу, это увидят
    // санитайзеры, а не совпадение с соседней памятью.
    std::vector<uint8_t> buf = v;
    unfuse_props_parse(buf.data(), buf.size(), &p);
    return p;
}

std::vector<uint8_t> note_with_features(uint32_t features) {
    std::vector<uint8_t> desc;
    put_prop(desc, UNFUSE_PROP_AARCH64_FEATURE_1_AND, u32bytes(features));
    std::vector<uint8_t> v;
    put_note(v, 4, "GNU", UNFUSE_NT_GNU_PROPERTY_TYPE_0, desc);
    return v;
}

}  // namespace

int main(int argc, char **argv) {
    printf("== разбор GNU property note ==\n");

    printf("  корректная заметка\n");
    {
        const UnfuseImageProps p = parse(note_with_features(UNFUSE_FEAT_BTI | UNFUSE_FEAT_PAC));
        check(p.has_note == 1, "заметка найдена");
        check((p.features & UNFUSE_FEAT_BTI) != 0, "бит BTI прочитан");
        check((p.features & UNFUSE_FEAT_PAC) != 0, "бит PAC прочитан");
        check((p.features & UNFUSE_FEAT_GCS) == 0, "чужой бит не появился");
    }
    {
        const UnfuseImageProps p = parse(note_with_features(0));
        check(p.has_note == 1 && p.features == 0, "нулевое слово возможностей");
    }

    printf("  чужие заметки\n");
    {
        std::vector<uint8_t> desc;
        put_prop(desc, UNFUSE_PROP_AARCH64_FEATURE_1_AND, u32bytes(1));
        std::vector<uint8_t> v;
        put_note(v, 4, "XNU", UNFUSE_NT_GNU_PROPERTY_TYPE_0, desc);
        check(parse(v).has_note == 0, "чужое имя заметки не считается");
    }
    {
        std::vector<uint8_t> desc;
        put_prop(desc, UNFUSE_PROP_AARCH64_FEATURE_1_AND, u32bytes(1));
        std::vector<uint8_t> v;
        put_note(v, 4, "GNU", 4 /* NT_GNU_BUILD_ID */, desc);
        check(parse(v).has_note == 0, "чужой тип заметки не считается");
    }
    {
        // Свойство есть, но не то, которое нужно.
        std::vector<uint8_t> desc;
        put_prop(desc, 0xc0008001u, u32bytes(0xff));
        std::vector<uint8_t> v;
        put_note(v, 4, "GNU", UNFUSE_NT_GNU_PROPERTY_TYPE_0, desc);
        const UnfuseImageProps p = parse(v);
        check(p.has_note == 1 && p.features == 0, "нужного свойства нет — слово пустое");
    }
    {
        // Два свойства: нужное вторым.
        std::vector<uint8_t> desc;
        put_prop(desc, 0xc0008001u, u32bytes(0xff));
        put_prop(desc, UNFUSE_PROP_AARCH64_FEATURE_1_AND, u32bytes(UNFUSE_FEAT_GCS));
        std::vector<uint8_t> v;
        put_note(v, 4, "GNU", UNFUSE_NT_GNU_PROPERTY_TYPE_0, desc);
        const UnfuseImageProps p = parse(v);
        check(p.features == UNFUSE_FEAT_GCS, "второе свойство прочитано");
    }

    printf("  обрезанные данные\n");
    {
        std::vector<uint8_t> v = note_with_features(1);
        for (int cut = 1; cut <= 12 && cut < static_cast<int>(v.size()); cut++) {
            std::vector<uint8_t> t(v.begin(), v.end() - cut);
            const UnfuseImageProps p = parse(t);
            if (p.has_note && p.features != 0) {
                check(false, "обрезанная заметка отдала слово возможностей");
                break;
            }
        }
        check(true, "обрезка до 12 байт не даёт слова возможностей");
    }
    {
        // Заявленное описание длиннее буфера.
        std::vector<uint8_t> v;
        put32(v, 4);
        put32(v, 0x1000);
        put32(v, UNFUSE_NT_GNU_PROPERTY_TYPE_0);
        v.push_back('G'); v.push_back('N'); v.push_back('U'); v.push_back(0);
        for (int i = 0; i < 16; i++) v.push_back(0);
        check(parse(v).has_note == 0, "заявленный размер больше буфера — не читаем");
    }
    {
        // Заявленное имя длиннее буфера.
        std::vector<uint8_t> v;
        put32(v, 0x1000);
        put32(v, 4);
        put32(v, UNFUSE_NT_GNU_PROPERTY_TYPE_0);
        for (int i = 0; i < 16; i++) v.push_back(0);
        check(parse(v).has_note == 0, "заявленное имя больше буфера — не читаем");
    }

    printf("  враждебные размеры\n");
    {
        std::vector<uint8_t> v;
        put32(v, 4);
        put32(v, 8);
        put32(v, UNFUSE_NT_GNU_PROPERTY_TYPE_0);
        v.push_back('G'); v.push_back('N'); v.push_back('U'); v.push_back(0);
        put32(v, UNFUSE_PROP_AARCH64_FEATURE_1_AND);
        put32(v, 0xffffffffu);   // свойство длиной 4 ГБ
        check(parse(v).has_note == 1 && parse(v).features == 0,
              "свойство на 4 ГБ не переполняет счётчик");
    }
    {
        std::vector<uint8_t> v;
        put32(v, 4);
        put32(v, 0xffffff00u);   // описание почти на 4 ГБ
        put32(v, UNFUSE_NT_GNU_PROPERTY_TYPE_0);
        v.push_back('G'); v.push_back('N'); v.push_back('U'); v.push_back(0);
        check(parse(v).has_note == 0, "описание на 4 ГБ отвергнуто");
    }
    {
        std::vector<uint8_t> v;
        put32(v, 0xffffffffu);
        put32(v, 0xffffffffu);
        put32(v, UNFUSE_NT_GNU_PROPERTY_TYPE_0);
        check(parse(v).has_note == 0, "оба размера по 4 ГБ отвергнуты");
    }

    printf("  мусор\n");
    {
        std::vector<uint8_t> v(64, 0xff);
        const UnfuseImageProps p = parse(v);
        check(p.has_note == 0 || p.features == 0, "0xff во всех байтах ничего не даёт");
    }
    {
        std::vector<uint8_t> v(64, 0);
        check(parse(v).has_note == 0, "нули ничего не дают");
    }
    {
        check(parse({}).has_note == 0, "пустой сегмент");
    }
    {
        UnfuseImageProps p;
        memset(&p, 0, sizeof p);
        unfuse_props_parse(nullptr, 0, &p);
        unfuse_props_parse(nullptr, 64, &p);
        unfuse_props_parse(reinterpret_cast<const void *>(&p), 0, &p);
        unfuse_props_parse(note_with_features(1).data(), 0, &p);
        check(p.has_note == 0, "нулевой указатель и нулевой размер не читаются");
    }

    if (argc > 1) {
        printf("  настоящая заметка: %s\n", argv[1]);
        FILE *f = fopen(argv[1], "rb");
        if (f == nullptr) {
            check(false, "дамп секции не открылся");
        } else {
            std::vector<uint8_t> v;
            uint8_t chunk[4096];
            size_t n;
            while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) v.insert(v.end(), chunk, chunk + n);
            fclose(f);
            const UnfuseImageProps p = parse(v);
            printf("      %zu байт, свойства: %s%s%s\n", v.size(),
                   (p.features & UNFUSE_FEAT_BTI) ? "BTI " : "",
                   (p.features & UNFUSE_FEAT_PAC) ? "PAC " : "",
                   (p.features & UNFUSE_FEAT_GCS) ? "GCS " : "");
            check(p.has_note == 1, "заметка из объектного файла найдена");
            check((p.features & UNFUSE_FEAT_BTI) != 0,
                  "объект с -mbranch-protection=standard объявляет BTI");
        }
    }

    printf("\nитог: %d сошлось, %d провалов\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
