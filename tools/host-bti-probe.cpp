/*
 * host-bti-probe.cpp — проверка ПРОВОДКИ hooks_bti_report(), а не разбора.
 *
 * Разбор заметки проверяет tools/gnu-props-selftest.cpp на синтетике. Здесь
 * проверяется то, что вокруг него: поиск нужного образа через dl_iterate_phdr
 * (libc по имени, свой модуль по адресу), согласованность возвращённого кода со
 * строкой и безопасность усечённого буфера. Гоняется на хосте, где libc заведомо
 * не AArch64 — то есть на образе, который ничего не объявляет, и на образе,
 * который объявляет чужое (у glibc своя заметка для своего набора команд).
 *
 * Вызывается только отчёт: hooks_install() пропатчил бы libc хоста.
 * Собирается через tools/test-gnu-props.sh; на Windows/MinGW не собирается вовсе
 * — там нет dlfcn.h и sys/mman.h, и скрипт пропускает этот пункт.
 *
 * Код возврата: 0 — всё сошлось, 1 — расхождение.
 */

#include <stdio.h>
#include <string.h>

#include "hook_libc.h"

namespace {

int g_fail = 0;
int g_pass = 0;

void check(bool ok, const char *what) {
    printf("    [%s] %s\n", ok ? "ок" : "ПРОВАЛ", what);
    if (ok) g_pass++;
    else g_fail++;
}

}  // namespace

int main() {
    printf("== проводка hooks_bti_report ==\n");

    char buf[256];
    const int rc = hooks_bti_report(buf, sizeof buf);
    printf("    вернулось %d: %s\n", rc, buf);

    check(rc == 0 || rc == 1 || rc == -1, "код возврата в допустимом наборе");
    check(strstr(buf, "libc: ") == buf, "строка начинается с libc");
    check(strstr(buf, "; модуль: ") != nullptr, "строка называет и модуль");
    check(strlen(buf) > 0, "строка не пуста");

    // Код и строка обязаны говорить одно и то же: на этом коде в unfuse_zygisk.cpp
    // висит ветка с ошибкой, и расхождение сделало бы её неверной.
    const bool says_bti = strstr(buf, "BTI") != nullptr;
    check((rc > 0) == says_bti, "код возврата согласован со строкой");

    // libc хоста существует всегда, значит образ должен быть найден.
    check(strstr(buf, "образ не найден") == nullptr, "libc найден в списке образов");

    printf("  усечённые буферы\n");
    for (size_t n = 1; n <= 16; n++) {
        char tiny[16];
        memset(tiny, 0x7f, sizeof tiny);
        hooks_bti_report(tiny, n);
        if (tiny[n - 1] != '\0' && n < sizeof tiny) {
            check(false, "усечённый буфер не завершён нулём");
            break;
        }
    }
    check(true, "усечённый буфер всегда завершается нулём");

    {
        char one[1] = {'x'};
        hooks_bti_report(one, 1);
        check(one[0] == '\0', "буфер в 1 байт становится пустой строкой");
    }
    {
        hooks_bti_report(nullptr, 0);
        check(true, "нулевой буфер не трогается");
    }

    printf("\nитог: %d сошлось, %d провалов\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
