#!/usr/bin/env python3
"""check-hook-names.py — сверка трёх списков имён целей хука.

Зачем. Об одном и том же наборе целей знают трое, и каждый ведёт свой список:

  * tools/verify-hook-targets.py   HOOK_NAMES — предполётная проверка по образу;
  * tools/hookselftest.cpp         kNames[]   — самотест на устройстве;
  * src/hook_libc.cpp              kHooks[]   — что патчится на самом деле.

Состав и порядок обязаны совпадать. verify и hookselftest перечисляют ВСЕ имена
(корни и переходники) и должны совпадать буквально; kHooks — только корни, и он
обязан быть их подпоследовательностью в том же порядке. Проверялось это глазами,
а зря: hooks_report() печатает по токену на элемент kHooks, поэтому добавленное
или переименованное имя сдвигает отчёт, а тест продолжает проходить — он про
новое имя просто не знает, и «покрыто 22» на деле означает 22 из 23.

Отсюда же ловится и обратный случай: имя осталось в тестах, но выпало из kHooks
(цель перестали патчить) — тогда его перестаёт кто-либо проверять.

Код возврата: 0 — списки сходятся; 1 — расхождение; 2 — список не разобран.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

NAME = r'"([^"]+)"'


def block(path, start_marker, end_marker):
    """Тело списка от маркера до закрывающего маркера."""
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    try:
        start = text.index(start_marker)
    except ValueError:
        return None
    end = text.find(end_marker, start + len(start_marker))
    if end < 0:
        return None
    return text[start:end]


def load():
    got = {}
    got["verify"] = block(os.path.join(ROOT, "tools", "verify-hook-targets.py"),
                          "HOOK_NAMES = [", "]")
    got["selftest"] = block(os.path.join(ROOT, "tools", "hookselftest.cpp"),
                            "const char *const kNames[] = {", "};")
    got["kHooks"] = block(os.path.join(ROOT, "src", "hook_libc.cpp"),
                          "const HookDef kHooks[] = {", "};")
    return got


def main():
    raw = load()
    missing = [k for k, v in raw.items() if v is None]
    if missing:
        for k in missing:
            print("не нашёл список %s" % k, file=sys.stderr)
        return 2

    names = {k: re.findall(NAME, v) for k, v in raw.items()}
    # В kHooks каждая строка — {"имя", reinterpret_cast<void *>(обработчик)},
    # поэтому регексп выше берёт только имя и обработчики не подмешивает.
    v, s, h = names["verify"], names["selftest"], names["kHooks"]

    for who, lst in (("verify", v), ("selftest", s), ("kHooks", h)):
        print("%-9s %2d: %s" % (who, len(lst), " ".join(lst)))
    print()

    bad = 0

    # Имя, выпавшее из разбора (опечатка в написании, лишний символ), исчезает
    # молча — и подпоследовательность тогда «сходится» на усечённом списке.
    # Считаем строки определений отдельно: их столько же, сколько обработчиков.
    handlers = raw["kHooks"].count("reinterpret_cast")
    if handlers != len(h):
        bad = 1
        print("ПРОВАЛ: в kHooks %d обработчиков, а имён разобрано %d"
              % (handlers, len(h)), file=sys.stderr)

    if len(set(v)) != len(v):
        print("ПРОВАЛ: HOOK_NAMES содержит повторы", file=sys.stderr)
        bad = 1
    if v != s:
        bad = 1
        print("ПРОВАЛ: HOOK_NAMES != kNames", file=sys.stderr)
        for n in sorted(set(v) ^ set(s)):
            print("    %s только в %s" %
                  (n, "verify" if n in v else "hookselftest"), file=sys.stderr)
        if set(v) == set(s):
            print("    состав тот же, порядок разошёлся", file=sys.stderr)

    # Подпоследовательность, а не подмножество: kHooks задаёт порядок патчей, и
    # он же порядок токенов в hooks_report().
    i = 0
    for n in h:
        while i < len(s) and s[i] != n:
            i += 1
        if i == len(s):
            print("ПРОВАЛ: kHooks не является подпоследовательностью kNames "
                  "начиная с %s" % n, file=sys.stderr)
            bad = 1
            break
        i += 1

    if not bad:
        print("списки сходятся: kHooks (%d) — подпоследовательность kNames (%d)"
              % (len(h), len(s)))
    return bad


if __name__ == "__main__":
    sys.exit(main())
