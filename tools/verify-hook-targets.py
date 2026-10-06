#!/usr/bin/env python3
"""
verify-hook-targets.py — предполётная проверка хука на РЕАЛЬНОЙ libc с устройства.

Хук правит входные точки функций в libc, записывая туда 20 байт:

    bti jc               ; 0xd50324df — площадка входа, обязательно ПЕРВОЙ
    ldr x17, #8          ; 0x58000051 — литерал ниже, PC-относительно
    br  x17              ; 0xd61f0220
    .quad <обработчик>   ; 8 байт по смещению 12

bti jc здесь не украшение. bionic 17 собран с -mbranch-protection=standard
(корни начинаются с paciasp, переходники с bti c), и загрузчик выставляет
PROT_BTI на образ, как только тот объявит GNU_PROPERTY_AARCH64_FEATURE_1_BTI.
Ни один образ Android этого не объявляет, поэтому сегодня это инертные hint'ы —
но если объявит, смещение 0 станет охраняемым входом, и косвенное обращение к
пропатченному корню обязано попасть на площадку. Поэтому площадка идёт первой:
патч, начинающийся с загрузки, упал бы до обработчика. Вариант jc, а не c,
потому что площадка должна принимать оба типа ветвления: вызов приходит с
BTYPE=call, а переход не через x16/x17 — с BTYPE=jump, который bti c отвергает.

Прежде чем ставить это на устройство, надо убедиться в следующем — и всё это
проверяется здесь по копии libc, снятой с устройства.

  1. Символ экспортирован и виден dlsym(RTLD_DEFAULT, имя). Нет символа — хук
     просто пропускается, это не ошибка, но знать надо заранее.

  2. Адрес выровнен по 4 байта. Иначе записать 32-битные инструкции нельзя.

  3. Функция не короче 20 байт. Это главная опасность: если функция короче,
     патч затрёт начало СЛЕДУЮЩЕЙ функции, и приложение упадёт в случайном
     месте. Размер берётся из st_size, а при st_size == 0 — как расстояние до
     следующего символа в той же секции.

  4. Функция не является «переходником» — телом из пары перестановок аргументов
     и одного безусловного перехода. Переходники всегда коротки (8–32 байта),
     поэтому их пропускают, а покрытие доказывается по .plt.

  5. Пропущенные имена действительно покрыты: цепочка безусловных переходов
     доводится до корня, а переход через .plt раскрывается по таблице релокаций
     .rela.plt (R_AARCH64_JUMP_SLOT) в имя символа. Если цепочка кончается не на
     корне — имя не покрыто, и это надо знать.

  6. Патч действительно кодируется как задумано: байты пишутся в КОПИЮ файла и
     дизассемблируются обратно.

  7. Патчи не наезжают друг на друга: расстояния между корнями сверяются с
     шириной патча.

Использование:
    tools/verify-hook-targets.py device/libc/libc-arm64.so [ещё файлы...]
    tools/verify-hook-targets.py --json out.json device/libc/*.so
    tools/verify-hook-targets.py --patch-copy /tmp/patched.so device/libc/libc-arm64.so

Код возврата: 0 — все цели пригодны или покрыты; 1 — есть непокрытая цель;
2 — ошибка разбора.
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

LDR_X17 = 0x58000051  # ldr x17, #8
BR_X17 = 0xD61F0220   # br  x17
BTI_JC = 0xD50324DF   # bti jc (hint #38)
PATCH_SIZE = 20

# Must match kHooks[] in src/hook_libc.cpp.
HOOK_NAMES = [
    "open", "open64", "openat", "openat64",
    "creat", "creat64", "__open_2", "__openat_2",
    "mkdir", "mkdirat",
    "chmod", "fchmod", "fchmodat",
    "rename", "renameat", "renameat2",
    "link", "linkat",
    "mkstemp", "mkostemp", "mkstemps", "mkostemps",
]

SHT_SYMTAB = 2
SHT_DYNSYM = 11
SHT_PROGBITS = 1

STT_FUNC = 2
STT_GNU_IFUNC = 10

STT_NAMES = {0: "NOTYPE", 1: "OBJECT", 2: "FUNC", 3: "SECTION", 4: "FILE",
             5: "COMMON", 6: "TLS", 10: "IFUNC"}
STB_NAMES = {0: "LOCAL", 1: "GLOBAL", 2: "WEAK"}

MACHINE_AARCH64 = 0xB7
MACHINE_ARM = 0x28


# Minimal parser (no pyelftools): exact symbol fields and .dynsym order (relocations index it).

class Sym:
    __slots__ = ("name", "value", "size", "info", "shndx", "bind", "type",
                 "section", "index")

    def __init__(self, name, value, size, info, shndx):
        self.name = name
        self.value = value
        self.size = size
        self.info = info
        self.shndx = shndx
        self.bind = info >> 4
        self.type = info & 0xF
        self.section = "?"
        self.index = -1


class Elf:
    """Минимальный читатель ELF32/ELF64: заголовок, секции, символы, релокации."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as fh:
            self.data = fh.read()
        d = self.data
        if len(d) < 64 or d[:4] != b"\x7fELF":
            raise ValueError("не ELF-файл")
        self.is64 = d[4] == 2
        self.endian = "<" if d[5] == 1 else ">"
        e = self.endian
        if self.is64:
            (self.etype, self.machine, _ver, self.entry, self.phoff, self.shoff,
             _flags, self.ehsize, self.phentsize, self.phnum,
             self.shentsize, self.shnum, self.shstrndx) = struct.unpack_from(
                e + "HHIQQQIHHHHHH", d, 16)
        else:
            (self.etype, self.machine, _ver, self.entry, self.phoff, self.shoff,
             _flags, self.ehsize, self.phentsize, self.phnum,
             self.shentsize, self.shnum, self.shstrndx) = struct.unpack_from(
                e + "HHIIIIIHHHHHH", d, 16)

        self.sections = []
        for i in range(self.shnum):
            off = self.shoff + i * self.shentsize
            if self.is64:
                (name, typ, flags, addr, offset, size, link, info,
                 align, entsize) = struct.unpack_from(e + "IIQQQQIIQQ", d, off)
            else:
                (name, typ, flags, addr, offset, size, link, info,
                 align, entsize) = struct.unpack_from(e + "IIIIIIIIII", d, off)
            self.sections.append({
                "index": i, "name_off": name, "type": typ, "flags": flags,
                "addr": addr, "offset": offset, "size": size, "link": link,
                "info": info, "align": align, "entsize": entsize,
            })

        self.shstrtab = b""
        if self.shstrndx < len(self.sections):
            s = self.sections[self.shstrndx]
            self.shstrtab = d[s["offset"]:s["offset"] + s["size"]]
        for s in self.sections:
            s["name"] = self._cstr(self.shstrtab, s["name_off"])

        self.segments = []
        for i in range(self.phnum):
            off = self.phoff + i * self.phentsize
            if self.is64:
                p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
                    struct.unpack_from(e + "IIQQQQQQ", d, off)
            else:
                p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
                    struct.unpack_from(e + "IIIIIIII", d, off)
            self.segments.append({
                "type": p_type, "flags": p_flags, "offset": p_offset,
                "vaddr": p_vaddr, "filesz": p_filesz, "memsz": p_memsz,
                "align": p_align,
            })

        self.symbols = []
        self.dynsym = []          # index order — relocations reference it
        self._read_symbols()
        self.rela_plt = {}        # GOT slot address -> symbol name
        self._read_rela_plt()

        self.plt_ranges = [(s["addr"], s["addr"] + s["size"])
                           for s in self.sections
                           if s["name"] in (".plt", ".plt.sec") and s["size"]]

    @staticmethod
    def _cstr(blob, off):
        if off >= len(blob):
            return ""
        end = blob.find(b"\0", off)
        if end < 0:
            end = len(blob)
        return blob[off:end].decode("utf-8", "replace")

    def _read_symbols(self):
        d = self.data
        e = self.endian
        for s in self.sections:
            if s["type"] not in (SHT_DYNSYM, SHT_SYMTAB):
                continue
            strtab = b""
            if s["link"] < len(self.sections):
                ls = self.sections[s["link"]]
                strtab = d[ls["offset"]:ls["offset"] + ls["size"]]
            entsize = s["entsize"] or (24 if self.is64 else 16)
            n = s["size"] // entsize
            is_dyn = s["type"] == SHT_DYNSYM
            for i in range(n):
                off = s["offset"] + i * entsize
                if self.is64:
                    name, info, _other, shndx, value, size = \
                        struct.unpack_from(e + "IBBHQQ", d, off)
                else:
                    name, value, size, info, _other, shndx = \
                        struct.unpack_from(e + "IIIBBH", d, off)
                sym = Sym(self._cstr(strtab, name), value, size, info, shndx)
                sym.section = s["name"]
                sym.index = i if is_dyn else -1
                self.symbols.append(sym)
                if is_dyn:
                    self.dynsym.append(sym)

    def _read_rela_plt(self):
        """Разбирает .rela.plt: адрес GOT-слота -> имя символа."""
        sec = next((s for s in self.sections if s["name"] == ".rela.plt"), None)
        if sec is None:
            return
        e = self.endian
        ent = sec["entsize"] or 24
        for i in range(sec["size"] // ent):
            off = sec["offset"] + i * ent
            r_offset, r_info, _addend = struct.unpack_from(e + "QQq", self.data, off)
            sym_idx = r_info >> 32
            if sym_idx < len(self.dynsym):
                self.rela_plt[r_offset] = self.dynsym[sym_idx].name

    def machine_name(self):
        return {MACHINE_AARCH64: "AArch64", MACHINE_ARM: "ARM",
                0x3E: "x86-64"}.get(self.machine, "0x%x" % self.machine)

    def type_name(self):
        return {0: "NONE", 1: "REL", 2: "EXEC", 3: "DYN", 4: "CORE"}.get(
            self.etype, str(self.etype))

    def vaddr_to_offset(self, vaddr):
        for p in self.segments:
            if p["type"] != 1:
                continue
            if p["vaddr"] <= vaddr < p["vaddr"] + p["filesz"]:
                return p["offset"] + (vaddr - p["vaddr"])
        for s in self.sections:
            if s["addr"] and s["addr"] <= vaddr < s["addr"] + s["size"]:
                return s["offset"] + (vaddr - s["addr"])
        return None

    def read(self, vaddr, n):
        off = self.vaddr_to_offset(vaddr)
        if off is None:
            return None
        return self.data[off:off + n]

    def word(self, vaddr):
        b = self.read(vaddr, 4)
        if b is None or len(b) < 4:
            return None
        return struct.unpack_from(self.endian + "I", b, 0)[0]

    def in_plt(self, addr):
        return any(lo <= addr < hi for lo, hi in self.plt_ranges)

    def func_at(self, addr):
        """Символ-функция, начинающийся ровно по этому адресу (для имени корня)."""
        for s in self.symbols:
            if s.value == addr and s.type in (STT_FUNC, STT_GNU_IFUNC):
                return s
        return None


# Same rules as src/hook_libc.cpp: mask 0x7c000000 matches B and BL; imm26 is a signed offset.

def decode_branch(insn, addr):
    if (insn & 0x7C000000) != 0x14000000:
        return None
    off = insn & 0x03FFFFFF
    if off & 0x02000000:
        off -= 0x04000000
    return addr + off * 4


def decode_adrp(insn, addr):
    if (insn & 0x9F000000) != 0x90000000:
        return None
    immlo = (insn >> 29) & 3
    immhi = (insn >> 5) & 0x7FFFF
    imm = (immhi << 2) | immlo
    if imm & (1 << 20):
        imm -= (1 << 21)
    return (addr & ~0xFFF) + (imm << 12)


def decode_ldr_uimm(insn):
    """LDR (unsigned offset, 64 бита): 1111 1001 01 imm12 Rn Rt."""
    if (insn & 0xFFC00000) != 0xF9400000:
        return None
    return ((insn >> 5) & 0x1F, insn & 0x1F, ((insn >> 10) & 0xFFF) * 8)


def plt_stub_symbol(elf, stub_addr):
    """Имя символа, на который указывает заглушка .plt по адресу stub_addr."""
    i0 = elf.word(stub_addr)
    i1 = elf.word(stub_addr + 4)
    if i0 is None or i1 is None:
        return None
    page = decode_adrp(i0, stub_addr)
    if page is None:
        return None
    d = decode_ldr_uimm(i1)
    if d is None:
        return None
    rn, rt, off = d
    if rn != 16 or rt != 17:      # expect adrp x16 / ldr x17
        return None
    return elf.rela_plt.get(page + off)


# Patch only when the function is >= PATCH_SIZE; everything else is diagnostics.
# A thunk is short (<= THUNK_MAX) and ends in an unconditional branch — only those get unwrapped.

THUNK_MAX = 32


def classify_tail(elf, addr, size):
    """Переходник ли функция: коротка и заканчивается безусловным переходом.

    У переходников bionic перестановки аргументов стоят в НАЧАЛЕ, а переход — в
    конце. Например creat (12 байт):

        84f68: mov w2, w1          ; аргументы
        84f6c: mov w1, #0x241
        84f70: b   open@plt        ; хвостовой переход

    Поэтому проверять первую инструкцию бесполезно — она не переход. Ищем только
    B (0010 0110), не BL: вызов с возвратом — это обычное тело.

    Возвращает (вид, адрес_цели).
    """
    if not size or size < 4:
        return "opaque", None
    last_addr = addr + size - 4
    w = elf.word(last_addr)
    if w is None:
        return "opaque", None
    if (w & 0xFC000000) != 0x14000000:
        return "body", None
    off = w & 0x03FFFFFF
    if off & 0x02000000:
        off -= 0x04000000
    return "thunk", last_addr + off * 4


def is_thunk(elf, addr, size):
    """Переходник ли функция: коротка (<= THUNK_MAX) И кончается переходом.

    Ровно то же условие использует движок (tail_call_target в
    src/hook_libc.cpp). Переходник, даже если он не короче патча (mkdir — ровно
    20 байт на 17), движок НЕ патчит: вызовы к нему и так приходят на пропатченный
    корень через .plt, а лишняя правка — лишний риск. Поэтому и здесь он не
    попадает в список патчей: проверка должна подтверждать тот план, который
    исполняется на устройстве, а не более широкий.
    """
    if not size or size > THUNK_MAX:
        return False
    kind, _ = classify_tail(elf, addr, size)
    return kind == "thunk"


def size_at(elf, addr):
    """Размер функции по адресу: st_size, иначе расстояние до следующего символа."""
    s = elf.func_at(addr)
    if s is not None and s.size:
        return s.size
    cands = [x.value for x in elf.symbols
             if x.type in (STT_FUNC, STT_GNU_IFUNC) and x.value > addr
             and (s is None or x.shndx == s.shndx)]
    return (min(cands) - addr) if cands else None


def body_calls_root(elf, addr, size, roots_by_addr):
    """Ищет в теле функции вызов (bl) на уже пропатченный корень.

    Нужно для mkstemp и родни: они переходят на длинную mktemp_internal, а та
    внутри себя зовёт open@plt. Размотать это хвостовой цепочкой нельзя, поэтому
    тело просматривается целиком.
    """
    if not size:
        return None
    for off in range(0, size - 3, 4):
        w = elf.word(addr + off)
        if w is None:
            return None
        if (w & 0xFC000000) != 0x94000000:      # BL only
            continue
        tgt = addr + off + ((w & 0x03FFFFFF) - 0x04000000
                            if (w & 0x02000000) else (w & 0x03FFFFFF)) * 4
        if tgt in roots_by_addr:
            return tgt
        if elf.in_plt(tgt):
            name = plt_stub_symbol(elf, tgt)
            if name is None:
                continue
            sym = next((s for s in elf.symbols if s.name == name and s.value), None)
            if sym is not None and sym.value in roots_by_addr:
                return sym.value
    return None


def chase(elf, addr, roots_by_addr, depth=0, trail=None, seen=None):
    """Идёт по цепочке хвостовых переходов до корня.

    Возвращает (kind, root_addr, trail); kind — root / indirect / uncovered /
    cycle / deep / unreadable.
    """
    trail = trail or []
    seen = seen or set()
    if addr in roots_by_addr:
        return "root", addr, trail
    if addr in seen:
        return "cycle", None, trail
    if depth > 8:
        return "deep", None, trail
    seen.add(addr)

    size = size_at(elf, addr)
    if size is None:
        return "unreadable", None, trail

    if size > THUNK_MAX:
        # Real function reached: nothing to unwind, but check for a call to a root.
        root = body_calls_root(elf, addr, size, roots_by_addr)
        if root is not None:
            return "indirect", root, trail
        return "uncovered", None, trail

    kind, tgt = classify_tail(elf, addr, size)
    if kind != "thunk":
        return "uncovered", None, trail

    if elf.in_plt(tgt):
        name = plt_stub_symbol(elf, tgt)
        if name is None:
            return "uncovered", None, trail + ["%s->.plt(неизв.)" % hex(addr)]
        sym = next((s for s in elf.symbols if s.name == name and s.value), None)
        if sym is None:
            return "uncovered", None, trail + ["%s->%s(нет адреса)" % (hex(addr), name)]
        return chase(elf, sym.value, roots_by_addr, depth + 1,
                     trail + ["%s->%s@plt" % (hex(addr), name)], seen)

    label = elf.func_at(tgt)
    shown = label.name if label else hex(tgt)
    return chase(elf, tgt, roots_by_addr, depth + 1,
                 trail + ["%s->%s" % (hex(addr), shown)], seen)


def analyse(elf, arch64=True):
    """Возвращает (записи по именам, набор корней, есть ли непокрытая цель)."""
    # "next symbol in same section" map — fallback size source.
    by_sec = {}
    for s in elf.symbols:
        if s.type in (STT_FUNC, STT_GNU_IFUNC) and s.value:
            by_sec.setdefault(s.shndx, []).append(s)
    for v in by_sec.values():
        v.sort(key=lambda x: x.value)

    def next_after(sym):
        for other in by_sec.get(sym.shndx, []):
            if other.value > sym.value:
                return other
        return None

    def find(name):
        return next((s for s in elf.symbols
                     if s.name == name and s.value
                     and s.type in (STT_FUNC, STT_GNU_IFUNC)), None)

    entries = {}
    for name in HOOK_NAMES:
        s = find(name)
        if s is None:
            entries[name] = {"sym": None, "role": "missing"}
            continue
        nxt = next_after(s)
        eff = s.size or ((nxt.value - s.value) if nxt else None)
        if not arch64:
            role = "foreign"
        elif eff is None:
            role = "unknown"
        elif eff < PATCH_SIZE:
            role = "short"
        elif is_thunk(elf, s.value, eff):
            role = "thunk"
        else:
            role = "root"
        entries[name] = {"sym": s, "eff": eff, "next": nxt.name if nxt else None,
                         "gap": (nxt.value - s.value) if nxt else None,
                         "role": role}

    roots_by_addr = {e["sym"].value for e in entries.values()
                     if e["role"] == "root" and e["sym"]}
    addr_name = {}
    for name, e in entries.items():
        if e["role"] == "root" and e["sym"]:
            addr_name.setdefault(e["sym"].value, name)

    rows = []
    bad = False
    for name in HOOK_NAMES:
        e = entries[name]
        if e["role"] == "missing":
            rows.append({"name": name, "status": "нет символа", "verdict": "skip",
                         "note": "dlsym вернёт nullptr — движок пропустит"})
            continue

        s = e["sym"]
        row = {"name": name, "value": s.value, "size": s.size, "eff": e["eff"],
               "gap": e["gap"], "next": e["next"],
               "type": STT_NAMES.get(s.type, str(s.type)),
               "bind": STB_NAMES.get(s.bind, str(s.bind)),
               "role": e["role"], "align": s.value % 4}
        notes = []

        if not arch64:
            notes.append("не AArch64 — правка входа не реализована")
            row["verdict"] = "skip"
            rows.append(_finish(row, notes))
            continue

        if s.type == STT_GNU_IFUNC:
            notes.append("IFUNC: st_value может указывать на резолвер")
            row["verdict"] = "BAD"
            bad = True
        if s.value % 4 != 0:
            notes.append("адрес не выровнен по 4 байта")
            row["verdict"] = "BAD"
            bad = True
        if s.bind != 1:
            notes.append("не GLOBAL (%s)" % STB_NAMES.get(s.bind, s.bind))

        if e["role"] == "root":
            row.setdefault("verdict", "ok")
            if row["verdict"] != "BAD":
                row["verdict"] = "ok"
            notes.append("патчится (%s байт)" % e["eff"])
        elif e["role"] == "unknown":
            notes.append("размер неизвестен — патч недопустим")
            row["verdict"] = "BAD"
            bad = True
        elif e["role"] in ("short", "thunk"):
            # Neither is patched; both rely on the call reaching a patched root on its own.
            kind, _ = classify_tail(elf, s.value, e["eff"])
            if e["role"] == "short" and kind != "thunk":
                notes.append("короче %d байт и не переходник — патч затрёт соседнюю"
                             % PATCH_SIZE)
                row["verdict"] = "BAD"
                bad = True
            else:
                res, root, trail = chase(elf, s.value, roots_by_addr)
                if res in ("root", "indirect"):
                    rname = addr_name.get(root, hex(root))
                    row["verdict"] = "ok"
                    how = "через" if res == "root" else "косвенно через"
                    why = "переходник" if e["role"] == "thunk" else "коротка"
                    notes.append("%s (%s байт) — пропуск, покрыт %s %s"
                                 % (why, e["eff"], how, rname))
                    if trail:
                        notes.append("цепочка: " + " ".join(trail))
                else:
                    row["verdict"] = "BAD"
                    bad = True
                    notes.append("%s байт — пропуск, но корень не найден (%s)"
                                 % (e["eff"], res))
                    if trail:
                        notes.append("цепочка: " + " ".join(trail))

        rows.append(_finish(row, notes))

    collisions = []
    addrs = sorted(roots_by_addr)
    for a, b in zip(addrs, addrs[1:]):
        if b - a < PATCH_SIZE:
            collisions.append((addr_name.get(a, hex(a)), addr_name.get(b, hex(b)), b - a))
            bad = True

    return rows, roots_by_addr, addr_name, collisions, bad


def _finish(row, notes):
    row["note"] = "; ".join(notes)
    return row


def triple_for(elf):
    if elf.machine == MACHINE_AARCH64:
        return "aarch64"
    if elf.machine == MACHINE_ARM:
        return "armv7"
    return None


def disasm(tool, path, addr, triple, count=4):
    end = addr + count * 4
    cmd = [tool, "-d", "--triple=%s" % triple,
           "--start-address=0x%x" % addr, "--stop-address=0x%x" % end, path]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    except Exception as exc:  # noqa: BLE001
        return ["<дизассемблер недоступен: %s>" % exc]
    return [ln.strip() for ln in r.stdout.splitlines() if ":\t" in ln]


def patch_and_show(elf, rows, patch_copy):
    """Пишет патч в КОПИЮ файла и возвращает список сделанных правок."""
    data = bytearray(elf.data)
    patched = []
    for row in rows:
        if row.get("verdict") != "ok" or "value" not in row:
            continue
        if row["role"] != "root":
            continue
        addr = row["value"]
        off = elf.vaddr_to_offset(addr)
        if off is None:
            continue
        before = bytes(data[off:off + PATCH_SIZE])
        # Same order as patch_entry(): literal, then the load, then the branch,
        # and the landing pad last — see there for why this order and no other.
        struct.pack_into(elf.endian + "Q", data, off + 12, addr + 12)
        struct.pack_into(elf.endian + "I", data, off + 4, LDR_X17)
        struct.pack_into(elf.endian + "I", data, off + 8, BR_X17)
        struct.pack_into(elf.endian + "I", data, off + 0, BTI_JC)
        patched.append({"name": row["name"], "addr": addr,
                        "before": before.hex(),
                        "after": bytes(data[off:off + PATCH_SIZE]).hex()})
    if patch_copy:
        os.makedirs(os.path.dirname(os.path.abspath(patch_copy)), exist_ok=True)
        with open(patch_copy, "wb") as fh:
            fh.write(data)
    return patched


def report(path, elf, rows, roots, addr_name, collisions, bad, patched, tool,
           verbose, disasm_path=None):
    print("=" * 78)
    print("файл:   %s" % path)
    print("машина: %s   класс: %s   тип: %s"
          % (elf.machine_name(), "ELF64" if elf.is64 else "ELF32", elf.type_name()))
    print("-" * 78)
    print("%-11s %-10s %5s %5s %-6s %-6s %s"
          % ("имя", "адрес", "size", "зазор", "тип", "итог", "примечание"))
    print("-" * 78)

    n_ok = n_skip = n_bad = 0
    for row in rows:
        if "value" not in row:
            print("%-11s %-10s %5s %5s %-6s %-6s %s"
                  % (row["name"], "-", "-", "-", "-", row["verdict"], row["note"]))
            n_skip += 1
            continue
        if row["verdict"] == "ok":
            n_ok += 1
        elif row["verdict"] == "BAD":
            n_bad += 1
        else:
            n_skip += 1
        print("%-11s 0x%08x %5s %5s %-6s %-6s %s"
              % (row["name"], row["value"],
                 row["size"] if row["size"] else "-",
                 row["gap"] if row["gap"] is not None else "-",
                 row["type"], row["verdict"], row["note"]))

    print("-" * 78)
    print("итог: ok=%d  пропущено=%d  нельзя=%d" % (n_ok, n_skip, n_bad))
    print("корней патчится: %d — %s"
          % (len(roots), ", ".join(sorted(addr_name.values()))))

    if collisions:
        print("НАЛОЖЕНИЕ патчей (ближе %d байт):" % PATCH_SIZE)
        for a, b, d in collisions:
            print("    %s и %s — %d байт" % (a, b, d))

    triple = triple_for(elf)
    if verbose and patched and triple and disasm_path:
        print()
        print("--- проверка кодирования патча (в копии) ---")
        for p in patched:
            print("  %s @ 0x%x" % (p["name"], p["addr"]))
            print("    до:    %s" % " ".join(p["before"][i:i + 8] for i in range(0, PATCH_SIZE * 2, 8)))
            print("    после: %s" % " ".join(p["after"][i:i + 8] for i in range(0, PATCH_SIZE * 2, 8)))
            for ln in disasm(tool, disasm_path, p["addr"], triple, PATCH_SIZE // 4):
                print("      %s" % ln)
    return n_ok, n_skip, n_bad


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help="скопированные с устройства .so")
    ap.add_argument("--json", metavar="PATH", help="сохранить отчёт в JSON")
    ap.add_argument("--patch-copy", metavar="PATH",
                    help="записать пропатченную копию первого файла")
    ap.add_argument("--objdump", metavar="PATH",
                    default=os.environ.get("LLVM_OBJDUMP", "llvm-objdump"))
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="без дизассемблирования")
    args = ap.parse_args()

    tool = args.objdump
    if not os.path.exists(tool):
        found = subprocess.run(["sh", "-c", "command -v %s" % tool],
                               capture_output=True, text=True).stdout.strip()
        tool = found or tool

    all_rows = {}
    any_bad = False
    tmpdir = None
    try:
        for i, path in enumerate(args.files):
            try:
                elf = Elf(path)
            except Exception as exc:  # noqa: BLE001
                print("ошибка разбора %s: %s" % (path, exc), file=sys.stderr)
                return 2

            rows, roots, addr_name, collisions, bad = analyse(
                elf, arch64=(elf.machine == MACHINE_AARCH64))
            any_bad = any_bad or bad

            patched = []
            disasm_path = None
            if not args.quiet:
                if tmpdir is None:
                    tmpdir = tempfile.mkdtemp(prefix="hookcheck.")
                disasm_path = os.path.join(tmpdir,
                                           os.path.basename(path) + ".patched")
                patched = patch_and_show(elf, rows, disasm_path)
                if i == 0 and args.patch_copy:
                    shutil.copyfile(disasm_path, args.patch_copy)
                    print("пропатченная копия: %s" % args.patch_copy)

            report(path, elf, rows, roots, addr_name, collisions, bad,
                   patched, tool, verbose=not args.quiet, disasm_path=disasm_path)
            print()
            all_rows[path] = rows

        if args.json:
            with open(args.json, "w") as fh:
                json.dump(all_rows, fh, indent=2, ensure_ascii=False)
            print("отчёт сохранён: %s" % args.json)

        return 1 if any_bad else 0
    finally:
        if tmpdir and not args.patch_copy:
            shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
