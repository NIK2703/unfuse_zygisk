#!/usr/bin/env python3
"""check-mount-modes.py — the module's mount-mode numbers against the releases.

MOUNT_MODE_EXTERNAL_* is the one protocol constant the module cannot derive from
the target: Zygote passes it in args->mount_external, and it is a copy of
IVold.REMOUNT_MODE_* made in the framework. Android 11 still carries the long
enum (READ 2, WRITE 3, LEGACY 4, INSTALLER 5, FULL 6, PASS_THROUGH 7,
ANDROID_WRITABLE 8); 12 dropped READ, WRITE, LEGACY and FULL and renumbered
INSTALLER to 2, PASS_THROUGH to 3 and ANDROID_WRITABLE to 4.

Two things have to hold on every release the module claims (android_ver.h):

  * the mode the module must reach is accepted — DEFAULT everywhere, and
    ANDROID_WRITABLE, which is 8 on 11 and 4 from 12;
  * every mode the framework actually isolates is accepted. The framework sets
    args->mount_storage_dirs from ProcessList.needsStorageDataIsolation(), which
    on every release excludes exactly NONE, INSTALLER and PASS_THROUGH. A mode
    left out of the module's set is a process that keeps the per-package tmpfs
    over Android/{data,obb} — the bug this check exists for: 11 hands out READ
    and WRITE to every legacy-storage app and the module used to decline both.

An accepted value the release cannot hand out, or hands out only as a mode the
framework does not isolate, is inert: the module writes JNI_FALSE where the
framework already left it false. That is what the second half reads out of the
framework sources — without them it says so instead of guessing.

The module's set is not read from a literal list: it is resolved from the
predicate the call site uses (`if (!clears_storage_dirs(mode)) return;`) plus the
helpers that predicate calls, so a rewrite that stops naming a mode fails here
rather than at runtime.

Sources: <refs>/vold-*/binder/android/os/IVold.aidl for the numbering,
<refs>/fwb-*/services/core/java/com/android/server/... for what is assignable,
src/unfuse_zygisk.cpp for what the module accepts.

Usage: tools/check-mount-modes.py [--refs DIR] [--src FILE]
Exit: 0 all releases covered, 1 a real discrepancy, 2 the check could not run.
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# (vold reference tree, framework reference tree or None, release number).
REFS = [
    ("vold-11", "fwb-r", 11),
    ("vold-12", "fwb-12", 12),
    ("vold-12l", None, 12),
    ("vold-13", None, 13),
    ("vold-14", "fwb-14", 14),
    ("vold-15", None, 15),
    ("vold-16", "fwb-16", 16),
    ("vold-17", None, 17),
]

# The reference trees are not in the repository (out/ is ignored): the check
# finds them where the project keeps them, and says so when it cannot.
DEFAULT_REFS = [
    os.path.join(ROOT, "out", "tmp", "aosp-ref"),
    os.path.join(os.path.dirname(ROOT), "aosp-ref"),
]

AIDL = os.path.join("binder", "android", "os", "IVold.aidl")
FW_SERVICE = os.path.join("services", "core", "java", "com", "android", "server",
                          "StorageManagerService.java")
FW_POLICIES = [
    os.path.join("services", "core", "java", "com", "android", "server", "appop",
                 "AppOpsService.java"),
    os.path.join("services", "core", "java", "com", "android", "server", "pm",
                 "PackageManagerService.java"),
]

# ProcessList.needsStorageDataIsolation() excludes these on every release, so the
# framework never sets mount_storage_dirs for them.
NOT_ISOLATED = {"NONE", "INSTALLER", "PASS_THROUGH"}

RE_AIDL = re.compile(r"const\s+int\s+REMOUNT_MODE_(\w+)\s*=\s*(\d+)\s*;")
RE_CONST = re.compile(r"constexpr\s+int\s+(kMountMode\w+)\s*=\s*(\d+)\s*;")
RE_FW_MODE = re.compile(r"(?:Zygote|StorageManager)\.MOUNT_(?:MODE_)?EXTERNAL_(\w+)")

# `bool f(int mode) { ... }` — the module's mode predicates. No nested braces in
# this file, so the body is what sits between the first pair.
RE_FN = re.compile(r"bool\s+(\w+)\s*\(\s*int\s+\w+\s*\)\s*\{([^{}]*)\}")
RE_IDENT = re.compile(r"\bkMountMode\w+\b")
RE_HELPER_CALL = re.compile(r"\b(\w+)\s*\(\s*mode\s*\)")
# The one place the decision is taken.
RE_CALLSITE = re.compile(r"if\s*\(\s*!\s*(\w+)\s*\(\s*mode\s*\)\s*\)\s*return\s*;")


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return None


def read_aidl(path):
    """{MODE_NAME: value} out of an IVold.aidl, or None when unreadable."""
    text = read_text(path)
    if text is None:
        return None
    out = {name: int(val) for name, val in RE_AIDL.findall(text)}
    return out or None


def read_assignable(fw_dir):
    """Mode names the framework can actually hand to Zygote, or None."""
    if fw_dir is None:
        return None
    names = set()
    found = False
    for rel in [FW_SERVICE] + FW_POLICIES:
        text = read_text(os.path.join(fw_dir, rel))
        if text is None:
            continue
        found = True
        names.update(RE_FW_MODE.findall(text))
    return names if found else None


def read_module(src):
    """({name: value}, {accepted values}) out of the module, or None."""
    text = read_text(src)
    if text is None:
        return None
    consts = {name: int(val) for name, val in RE_CONST.findall(text)}

    fns = {name: body for name, body in RE_FN.findall(text)}

    m = RE_CALLSITE.search(text)
    if m is None:
        return None
    accepted = set()
    seen = set()
    stack = [m.group(1)]
    while stack:
        fn = stack.pop()
        if fn in seen:
            continue
        seen.add(fn)
        body = fns.get(fn)
        if body is None:
            # The call site names something that is not a parsed predicate:
            # refuse to guess what it accepts.
            return None
        for ident in RE_IDENT.findall(body):
            if ident in consts:
                accepted.add(consts[ident])
        stack.extend(RE_HELPER_CALL.findall(body))
    return consts, accepted


def pick_refs(explicit):
    if explicit:
        return explicit
    for cand in DEFAULT_REFS:
        if os.path.isdir(cand):
            return cand
    return DEFAULT_REFS[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--refs", default=None,
                    help="каталог с vold-*/ и fwb-*/ (по умолчанию "
                         + " или ".join(DEFAULT_REFS) + ")")
    ap.add_argument("--src", default=os.path.join(ROOT, "src", "unfuse_zygisk.cpp"))
    args = ap.parse_args()

    refs = pick_refs(args.refs)
    if not os.path.isdir(refs):
        print(f"нет каталога ссылок: {refs}", file=sys.stderr)
        return 2

    parsed = read_module(args.src)
    if parsed is None:
        print(f"разбор {args.src} сломался — не нашёл место, где решается "
              f"режим (ожидается `if (!f(mode)) return;` с разбираемыми "
              f"предикатами)", file=sys.stderr)
        return 2
    consts, accepted = parsed

    default = consts.get("kMountModeExternalDefault")
    if default is None or not accepted:
        print("разбор констант модуля сломался — не нашёл DEFAULT или "
              "принимаемых режимов", file=sys.stderr)
        return 2

    print("модуль принимает:")
    for name in sorted(consts):
        print(f"    {name} = {consts[name]}")
    print(f"    -> {sorted(accepted)}")
    print()

    print("релиз  ANDROID_WRITABLE  DEFAULT  изолируемых  вердикт")
    print("-" * 88)
    bad = 0
    seen = 0
    unassigned = 0
    unchecked = 0
    for vold_ref, fw_ref, release in REFS:
        aidl = read_aidl(os.path.join(refs, vold_ref, AIDL))
        if aidl is None:
            print(f"{release:<6} (нет {vold_ref}/{AIDL})")
            continue
        seen += 1

        by_value = {}
        for name, val in aidl.items():
            by_value.setdefault(val, name)

        aw = aidl.get("ANDROID_WRITABLE")
        df = aidl.get("DEFAULT")
        assignable = read_assignable(os.path.join(refs, fw_ref) if fw_ref else None)

        problems = []
        if aw is None:
            problems.append("нет ANDROID_WRITABLE в aidl")
        elif aw not in accepted:
            problems.append(f"модуль не примет {aw}")
        if df != default:
            problems.append(f"DEFAULT {df} != {default}")

        # Completeness: every mode this release isolates has to be accepted, or
        # its process keeps the tmpfs over Android/{data,obb}.
        coverage = "—"
        if assignable is None:
            unchecked += 1
        else:
            isolated = {name: val for name, val in aidl.items()
                        if name not in NOT_ISOLATED and name in assignable}
            missing = [f"{name} {val}" for name, val in sorted(isolated.items(),
                                                               key=lambda kv: kv[1])
                       if val not in accepted]
            coverage = f"{len(isolated) - len(missing)}/{len(isolated)}"
            if missing:
                problems.append("не принимает изолируемые: " + ", ".join(missing))

        # Every other accepted value must be one this release cannot hand out, or
        # hands out only as a mode the framework does not isolate.
        notes = []
        for val in sorted(accepted):
            if val in (aw, df):
                continue
            name = by_value.get(val)
            if name is None:
                notes.append(f"{val} вне enum")
            elif name in NOT_ISOLATED:
                notes.append(f"{val} = {name}, не изолируется — no-op")
            elif assignable is None:
                notes.append(f"{val} = {name}, выдача не проверена")
                unassigned += 1
            elif name in assignable:
                notes.append(f"{val} = {name}, изолируется — и принимается")
            else:
                notes.append(f"{val} = {name}, не выдаётся")

        verdict = "сходится" if not problems else "РАСХОЖДЕНИЕ: " + "; ".join(problems)
        if notes:
            verdict += "  [" + "; ".join(notes) + "]"
        if problems:
            bad += 1
        print(f"{release:<6} {str(aw):<17} {str(df):<8} {coverage:<11} {verdict}")

    print()
    if seen == 0:
        print("ни одного aidl не прочитано", file=sys.stderr)
        return 2
    if bad:
        print(f"итог: {seen} релизов, {bad} с расхождением", file=sys.stderr)
        return 1
    tail = ""
    if unchecked:
        tail += (f"; полнота не проверена на {unchecked} релизах "
                 f"(нет исходников framework)")
    if unassigned:
        tail += f"; выдача не проверена на {unassigned} парах"
    print(f"итог: {seen} релизов, всё сходится{tail}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
