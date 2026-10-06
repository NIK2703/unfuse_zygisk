#!/usr/bin/env python3
"""check-mount-modes.py — the module's mount-mode numbers against the releases.

MOUNT_MODE_EXTERNAL_* is the one protocol constant the module cannot derive from
the target: Zygote passes it in args->mount_external, and it is a copy of
IVold.REMOUNT_MODE_* made in the framework. Android 11 still carries the long
enum and numbers ANDROID_WRITABLE 8; 12 dropped READ, WRITE, LEGACY and FULL and
renumbered it to 4. So the set of values the module accepts has to cover every
release it claims (android_ver.h), and a release that renumbers again has to show
up here rather than as an android_writable process the module silently declines.

Accepting a value the module does not mean is only safe when that value is a
DIFFERENT mode nobody hands out on that release — 11 numbers LEGACY 4, and 11's
StorageManagerService never returns LEGACY. That is what the second half of the
check reads out of the framework sources; without them it says so instead of
guessing.

Sources: aosp-ref/vold-*/binder/android/os/IVold.aidl for the numbering,
aosp-ref/fwb-*/services/core/java/com/android/server/... for what is assignable,
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
    ("vold-12", None, 12),
    ("vold-12l", None, 12),
    ("vold-13", None, 13),
    ("vold-14", "fwb-14", 14),
    ("vold-15", None, 15),
    ("vold-16", "fwb-16", 16),
    ("vold-17", None, 17),
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

RE_AIDL = re.compile(r"const\s+int\s+REMOUNT_MODE_(\w+)\s*=\s*(\d+)\s*;")
RE_CONST = re.compile(r"constexpr\s+int\s+(kMountMode\w+)\s*=\s*(\d+)\s*;")
RE_FW_MODE = re.compile(r"(?:Zygote|StorageManager)\.MOUNT_(?:MODE_)?EXTERNAL_(\w+)")


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
    base = os.path.join(fw_dir)
    names = set()
    found = False
    for rel in [FW_SERVICE] + FW_POLICIES:
        text = read_text(os.path.join(base, rel))
        if text is None:
            continue
        found = True
        names.update(RE_FW_MODE.findall(text))
    return names if found else None


def read_module(src):
    text = read_text(src)
    if text is None:
        return None
    return {name: int(val) for name, val in RE_CONST.findall(text)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--refs", default=os.path.join(os.path.dirname(ROOT), "aosp-ref"))
    ap.add_argument("--src", default=os.path.join(ROOT, "src", "unfuse_zygisk.cpp"))
    args = ap.parse_args()

    if not os.path.isdir(args.refs):
        print(f"нет каталога ссылок: {args.refs}", file=sys.stderr)
        return 2

    mod = read_module(args.src)
    if mod is None:
        print(f"нет исходника модуля: {args.src}", file=sys.stderr)
        return 2

    accepted = {val for name, val in mod.items()
                if name.startswith("kMountModeExternalAndroidWritable")}
    default = mod.get("kMountModeExternalDefault")
    if default is None or not accepted:
        print("разбор констант модуля сломался — не нашёл DEFAULT или "
              "ANDROID_WRITABLE", file=sys.stderr)
        return 2

    print("модуль принимает:")
    for name in sorted(mod):
        print(f"    {name} = {mod[name]}")
    print(f"    -> DEFAULT={default}, ANDROID_WRITABLE={sorted(accepted)}")
    print()

    print("релиз  ANDROID_WRITABLE  DEFAULT  вердикт")
    print("-" * 74)
    bad = 0
    seen = 0
    unchecked = 0
    for vold_ref, fw_ref, release in REFS:
        aidl = read_aidl(os.path.join(args.refs, vold_ref, AIDL))
        if aidl is None:
            print(f"{release:<6} (нет {vold_ref}/{AIDL})")
            continue
        seen += 1

        by_value = {}
        for name, val in aidl.items():
            by_value.setdefault(val, name)

        aw = aidl.get("ANDROID_WRITABLE")
        df = aidl.get("DEFAULT")
        assignable = read_assignable(
            os.path.join(args.refs, fw_ref) if fw_ref else None)

        problems = []
        if aw is None:
            problems.append("нет ANDROID_WRITABLE в aidl")
        elif aw not in accepted:
            problems.append(f"модуль не примет {aw}")
        if df != default:
            problems.append(f"DEFAULT {df} != {default}")

        # Every other value the module accepts must be a mode this release
        # cannot hand out, or the module would read that mode as
        # ANDROID_WRITABLE.
        notes = []
        for val in sorted(accepted):
            if val == aw:
                continue
            name = by_value.get(val)
            if name is None:
                notes.append(f"{val} вне enum — не встречается")
                continue
            if assignable is None:
                notes.append(f"{val} = {name}, выдача не проверена (нет исходников)")
                unchecked += 1
            elif name in assignable:
                problems.append(f"{val} = {name}, и её выдают")
            else:
                notes.append(f"{val} = {name}, не выдаётся")

        verdict = "сходится" if not problems else "РАСХОЖДЕНИЕ: " + "; ".join(problems)
        if notes:
            verdict += "  [" + "; ".join(notes) + "]"
        if problems:
            bad += 1
        print(f"{release:<6} {str(aw):<17} {str(df):<8} {verdict}")

    print()
    if seen == 0:
        print("ни одного aidl не прочитано", file=sys.stderr)
        return 2
    if bad:
        print(f"итог: {seen} релизов, {bad} с расхождением", file=sys.stderr)
        return 1
    print(f"итог: {seen} релизов, всё сходится"
          + (f"; выдача не проверена на {unchecked} парах (нет исходников framework)"
             if unchecked else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
