#!/usr/bin/env python3
"""check-release-table.py — сверка таблицы релизов с эталонными образами.

Зачем. Колонки `installed` / `installed_arm` в src/android_ver.h — это
утверждение о НАСТОЯЩЕМ образе: «на этой сборке список целей покрывает ровно
столько». Проверялось оно руками, один раз на релиз. Между тем именно оно
превращает лог в диагноз: при расхождении unfuse_zygisk.cpp пишет, что список
целей разошёлся с проверенным для этой сборки. Если сама таблица протухнет
(цель усохла ниже порога, символ пропал, обработчик переписали), на устройстве
это будет выглядеть как дрейф сборки, а не как ошибка в таблице, — и
разбираться придётся с ложной стороны.

Здесь связь замыкается на хосте: для каждой строки запускается
tools/verify-hook-targets.py на обоих образах, которые строка называет (arm64 и
arm32), и число покрытых целей сравнивается с соответствующей колонкой.
Считаются именно строки «патчится», включая алиасы (open64/open делят адрес),
потому что hooks_install() считает Ok + Alias — то есть ровно их, и ровно те
имена, что стоят в kHooks (MODULE_HOOKS в верifier'е).

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

# Те же релизы для armeabi-v7a. libc-arm.so — база (аналог libc-arm64.so).
SDK_IMAGE_ARM = {
    30: "device/libc/libc-arm-a11.so",
    31: "device/libc/libc-arm-a12.so",
    32: "device/libc/libc-arm-a12l.so",
    33: "device/libc/libc-arm-a13.so",
    34: "device/libc/libc-arm-a14.so",
    35: "device/libc/libc-arm-a15.so",
    36: "device/libc/libc-arm.so",
    37: "device/libc/libc-arm-a17.so",
}

ROW = re.compile(
    r'\{\s*(\d+),\s*(\d+),\s*"([^"]+)",\s*(\d+),\s*(\d+),\s*"([^"]+)"\s*\}')
PATCHED = re.compile(r"^\S+\s+0x[0-9a-f]+\s")
ROOTS = re.compile(r"корней патчится: (\d+)")


def rows_from_header(path):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    # Only the initialiser list, so the comment block above cannot match.
    start = text.index("UNFUSE_VERSIONS[] = {")
    end = text.index("};", start)
    return ROW.findall(text[start:end])


def measure(rel_img, verbose):
    """Гоняет верifier на образе и возвращает (n_патчится, корней) или None."""
    img = os.path.join(ROOT, rel_img)
    if not os.path.exists(img):
        return None
    # -q: дизассемблирование здесь не нужно, а именно оно и медленное.
    proc = subprocess.run(
        [sys.executable, os.path.join(HERE, "verify-hook-targets.py"), "-q", rel_img],
        cwd=ROOT, capture_output=True, text=True)
    out = proc.stdout
    if verbose:
        print(out)
    if proc.returncode not in (0, 1) or not out:
        return "error"
    # Строки таблицы целей, помеченные «патчится». Алиасы (open64/open) идут
    # отдельными строками с тем же адресом — их и считает hooks_install().
    #
    # «патчится», а не «патчится (»: стаб сисколла __openat находится по
    # форме, а не по символу, и верifier помечает его «патчится по форме
    # (24 байт)» — с этой строкой таблица сходится ровно (9 на 11–13, 8 на
    # 14–17 у arm64; 9 на всех у arm32), а без неё расходилась на всех восьми.
    n = sum(1 for l in out.splitlines()
            if PATCHED.match(l) and "патчится" in l)
    m = ROOTS.search(out)
    roots = int(m.group(1)) if m else -1
    return n, roots


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

    print("%-4s %-4s %-15s %11s %11s   %s" %
          ("sdk", "вып", "кодовое имя", "arm64", "arm32", "вердикт"))
    print("-" * 84)

    failed = 0
    for sdk, rel, name, installed, installed_arm, _vold in rows:
        sdk, installed, installed_arm = int(sdk), int(installed), int(installed_arm)

        img64 = SDK_IMAGE.get(sdk)
        imgarm = SDK_IMAGE_ARM.get(sdk)
        if img64 is None or imgarm is None:
            print("%-4d %-4d %-15s %11d %11d   нет соответствия sdk -> образ"
                  % (sdk, int(rel), name, installed, installed_arm))
            failed += 1
            continue

        r64 = measure(img64, args.verbose)
        rarm = measure(imgarm, args.verbose)

        notes = []
        ok = True
        cells = []
        for tag, res, want in (("arm64", r64, installed), ("arm32", rarm, installed_arm)):
            if res is None:
                cells.append("нет образа")
                notes.append("%s: образа нет в дереве" % tag)
                ok = False
                continue
            if res == "error":
                cells.append("не отработал")
                notes.append("%s: верifier не отработал" % tag)
                ok = False
                continue
            n, roots = res
            cells.append("%d/%d" % (n, want))
            # Строки «патчится» и собственная сводка верifier'а обязаны совпасть:
            # если разойдутся между собой, протухла не таблица, а один из
            # счётчиков, и тогда сверка с колонкой уже ничего не доказывает.
            if not (n == want == roots):
                ok = False
                if n != roots:
                    notes.append("%s: счётчики верifier'а разошлись (строк %d, корней %d)"
                                 % (tag, n, roots))
                else:
                    notes.append("%s: ожидалось %d, верifier видит %d" % (tag, want, n))

        if not ok:
            failed += 1
        verdict = "сходится" if ok else "РАСХОДИТСЯ"
        if notes:
            verdict += " (" + "; ".join(notes) + ")"
        print("%-4d %-4d %-15s %11s %11s   %s"
              % (sdk, int(rel), name, cells[0], cells[1], verdict))

    print()
    if failed:
        print("итог: %d строк, %d расхождений" % (len(rows), failed))
        return 1
    print("итог: %d строк, всё сходится" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
