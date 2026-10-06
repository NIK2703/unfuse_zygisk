#!/usr/bin/env python3
"""check-release-table.py — сверка таблицы релизов с эталонными образами.

Зачем. Колонка `installed` в src/android_ver.h — это утверждение о НАСТОЯЩЕМ
образе: «на этой сборке список целей покрывает ровно столько». Проверялось оно
руками, один раз на релиз. Между тем именно оно превращает лог в диагноз: при
расхождении unfuse_zygisk.cpp пишет, что список целей разошёлся со проверенным
для этой сборки. Если сама таблица протухнет (цель усохла ниже порога, символ
пропал, обработчик переписали), на устройстве это будет выглядеть как дрейф
сборки, а не как ошибка в таблице, — и разбираться придётся с ложной стороны.

Здесь связь замыкается на хосте: для каждой строки запускается
tools/verify-hook-targets.py на том образе, который строка называет, и число
покрытых целей сравнивается с `installed`. Считаются именно строки «патчится»,
включая алиасы (open64/open делят адрес), потому что hooks_install() считает
Ok + Alias — то есть ровно их.

Соответствие sdk -> образ задано здесь, а не выведено: у 16 и 17 файлы названы
по-разному (libc-arm64.so против libc-arm64-a17.so), и угадывать это по имени
нельзя. Строка без образа в дереве пропускается с пометкой, но не молча.

Использование: tools/check-release-table.py [-v]
    -v   печатать полный отчёт верifier'а по каждому образу

Код возврата 1, если хоть одна строка разошлась или образа нет.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Один образ на строку. 16 назван без суффикса: он и есть базовая версия.
SDK_IMAGE = {
    30: "device/libc/libc-arm64-a11.so",
    31: "device/libc/libc-arm64-a12.so",
    32: "device/libc/libc-arm64-a12l.so",
    33: "device/libc/libc-arm64-a13.so",
    34: "device/libc/libc-arm64-a14.so",
    35: "device/libc/libc-arm64-a15.so",
    36: "device/libc/libc-arm64.so",
    37: "device/libc/libc-arm64-a17.so",
}

ROW = re.compile(
    r'\{\s*(\d+),\s*(\d+),\s*"([^"]+)",\s*(\d+),\s*"([^"]+)"\s*\}')
PATCHED = re.compile(r"^\S+\s+0x[0-9a-f]+\s")
ROOTS = re.compile(r"корней патчится: (\d+)")


def rows_from_header(path):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    # Only the initialiser list, so the comment block above cannot match.
    start = text.index("UNFUSE_VERSIONS[] = {")
    end = text.index("};", start)
    return ROW.findall(text[start:end])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="показывать отчёт верifier'а целиком")
    args = ap.parse_args()

    hdr = os.path.join(ROOT, "src", "android_ver.h")
    rows = rows_from_header(hdr)
    if not rows:
        print("не разобрать UNFUSE_VERSIONS в %s" % hdr, file=sys.stderr)
        return 2

    print("%-4s %-4s %-15s %8s %8s   %s" %
          ("sdk", "вып", "кодовое имя", "таблица", "образ", "вердикт"))
    print("-" * 74)

    failed = 0
    for sdk, rel, name, installed, _vold in rows:
        sdk, installed = int(sdk), int(installed)
        rel_img = SDK_IMAGE.get(sdk)

        if rel_img is None:
            print("%-4d %-4d %-15s %8d %8s   нет соответствия sdk -> образ"
                  % (sdk, int(rel), name, installed, "-"))
            failed += 1
            continue

        img = os.path.join(ROOT, rel_img)
        if not os.path.exists(img):
            print("%-4d %-4d %-15s %8d %8s   образа нет в дереве"
                  % (sdk, int(rel), name, installed, "-"))
            failed += 1
            continue

        # -q: дизассемблирование здесь не нужно, а именно оно и медленное.
        proc = subprocess.run(
            [sys.executable, os.path.join(HERE, "verify-hook-targets.py"), "-q", rel_img],
            cwd=ROOT, capture_output=True, text=True)
        out = proc.stdout
        if args.verbose:
            print(out)

        # Строки таблицы целей, помеченные «патчится». Алиасы (open64/open) идут
        # отдельными строками с тем же адресом — их и считает hooks_install().
        n = sum(1 for l in out.splitlines()
                if PATCHED.match(l) and "патчится (" in l)
        m = ROOTS.search(out)
        roots = int(m.group(1)) if m else -1

        if proc.returncode not in (0, 1) or not out:
            print("%-4d %-4d %-15s %8d %8s   верifier не отработал"
                  % (sdk, int(rel), name, installed, "-"))
            failed += 1
            continue

        ok = (n == installed)
        if not ok:
            failed += 1
        print("%-4d %-4d %-15s %8d %8d   %s, корней %d"
              % (sdk, int(rel), name, installed, n,
                 "сходится" if ok else "РАСХОДИТСЯ", roots))

    print()
    if failed:
        print("итог: %d строк, %d расхождений" % (len(rows), failed))
        return 1
    print("итог: %d строк, всё сходится" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
