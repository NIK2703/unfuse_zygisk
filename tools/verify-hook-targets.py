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

Разбор идёт по ABI, и план патча у них разный. AArch64: 20 байт, `bti jc` +
`ldr x17,#8` + `br x17`, цепочка через .rela.plt. armeabi-v7a: 8 байт, две
формы по режиму цели (`ldr.w pc,[pc,#0]` для Thumb-2, `ldr pc,[pc,#-4]` для
ARM), цепочка через `__ThumbV7PILongThunk_*` -> .plt -> .rel.plt. Обе ABI
считаются поддерживаемыми, и обе обязаны давать код 0.

Образ с ЛЮБОЙ другой машиной (x86 и т.п.) — это код 1, а не 0: правка входа
для него не реализована, поэтому ни одна цель на нём не патчится. Строка
«ABI не поддерживается» в таблице была и раньше, но итог и код возврата
оставались нулевыми, то есть скрипт, читающий код, видел зелёный свет там,
где не покрыто ничего.
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

LDR_X17 = 0x58000051  # ldr x17, #8
BR_X17 = 0xD61F0220   # br  x17
BTI_JC = 0xD50324DF   # bti jc (hint #38)
PATCH_SIZE = 20

# Must match kHooks[] in src/hook_libc.cpp (корни) плюс покрытые переходники.
# open/open64/openat/openat64 сняты с модуля: их работу берёт стаб сисколла
# __openat, найденный по форме (см. ниже openat_stub_root). Они здесь не
# перечислены — иначе скрипт счёл бы их корнями и требовал патчить по имени.
HOOK_NAMES = [
    "creat", "creat64", "__open_2", "__openat_2",
    "mkdir", "mkdirat",
    "chmod", "fchmod", "fchmodat",
    "rename", "renameat", "renameat2",
    "link", "linkat",
    "mkstemp", "mkostemp", "mkstemps", "mkostemps",
]

# Имена, которые модуль РЕАЛЬНО патчит, — ровно kHooks в src/hook_libc.cpp (плюс
# форм-цель __openat, добавляемая ниже отдельно). HOOK_NAMES шире: это «все входы,
# которые надо покрыть», а корнем объявляется только то, что стоит в kHooks.
#
# Разделение нужно из-за ARM32. Там rename и link — тела на 28 байт
# (`push; …; blx …@plt; pop {r7,pc}`), а не хвостовые переходники, как на
# AArch64, поэтому по одной лишь форме они «корни», хотя модуль их не патчит:
# они зовут renameat2@plt / linkat@plt, то есть покрыты косвенно. Без этого
# списка верifier объявлял бы на ARM32 11 корней вместо 9 и расходился бы с
# hooks_install() (и с колонкой installed в src/android_ver.h).
MODULE_HOOKS = {
    "__open_2", "__openat_2", "mkdirat", "fchmod", "fchmodat",
    "renameat", "renameat2", "linkat",
}

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
        """Таблица релокаций PLT: адрес GOT-слота -> имя символа.

        Два формата, и они не взаимозаменяемы:
          .rela.plt — RELA (AArch64): 24 байта, r_info 64 бита, индекс = r_info >> 32;
          .rel.plt  — REL (ARM32):     8 байт, r_info 32 бита, индекс = r_info >> 8.
        Именно на .rel.plt держится вся размотка цепочек на armeabi-v7a: переходник
        Thumb идёт на .plt-заглушку, а та — через GOT в этот символ.
        """
        sec = next((s for s in self.sections if s["name"] == ".rela.plt"), None)
        if sec is not None:
            e = self.endian
            ent = sec["entsize"] or 24
            for i in range(sec["size"] // ent):
                off = sec["offset"] + i * ent
                r_offset, r_info, _addend = struct.unpack_from(e + "QQq", self.data, off)
                sym_idx = r_info >> 32
                if sym_idx < len(self.dynsym):
                    self.rela_plt[r_offset] = self.dynsym[sym_idx].name
            return

        sec = next((s for s in self.sections if s["name"] == ".rel.plt"), None)
        if sec is None:
            return
        e = self.endian
        ent = sec["entsize"] or 8
        for i in range(sec["size"] // ent):
            off = sec["offset"] + i * ent
            r_offset, r_info = struct.unpack_from(e + "II", self.data, off)
            sym_idx = r_info >> 8
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

    def half(self, vaddr):
        """Полуслово — единица разбора Thumb-2, где инструкции 2 или 4 байта."""
        b = self.read(vaddr, 2)
        if b is None or len(b) < 2:
            return None
        return struct.unpack_from(self.endian + "H", b, 0)[0]

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


# ---------------------------------------------------------------------------
# armeabi-v7a: ARM и Thumb-2 живут в ОДНОМ образе, и режим цели виден по
# младшему биту адреса (dlsym отдаёт Thumb-функцию с битом 0). Поэтому патч
# входа здесь 8 байт, а не 20, и у него две формы:
#
#   Thumb-2 (бит 0 = 1):  ldr.w pc, [pc, #0]   ; F8DF F000 — литерал по entry+4
#                         .word <обработчик>
#   ARM     (бит 0 = 0):  ldr   pc, [pc, #-4]  ; E51FF004 — литерал по entry+4
#                         .word <обработчик>
#
# Обе требуют, чтобы entry был выровнен по 4: LDR с базой PC считает адрес как
# Align(PC,4)+imm, и на 2-выровненном начале литерал прочитался бы не оттуда.
# Ни BTI, ни PAC на AArch32 нет, поэтому площадки-паддинга здесь нет вовсе.
#
# Цепочка покрытия тоже другая. Thumb-обёртка (creat, mkdir, chmod, rename,
# link, mkstemp…) кончается `b.w` на `__ThumbV7PILongThunk_<имя>`; тот — это
# `movw r12,#lo; movt r12,#hi; add r12,pc; bx r12`, то есть прыжок на
# .plt-заглушку ARM; а заглушка через GOT (.rel.plt, R_ARM_JUMP_SLOT) идёт в
# настоящий символ. Это ровно аналог .plt-цепочки AArch64 и разматывается так
# же — до корня. Проверено на образе 16: open -> blx .plt(0xb2450) -> __openat.

PATCH_SIZE_ARM = 8
# Полуслова Thumb-2 идут в памяти В ПОРЯДКЕ ПОЛУСЛОВ: первое — по младшему
# адресу. Как 32-битное little-endian слово это hw0 | (hw1 << 16), а не
# «склеенная» запись hw0hw1: `ldr.w pc,[pc,#0]` — это hw0=0xF8DF, hw1=0xF000,
# то есть слово 0xF000F8DF. Обратный порядок даёт 0xF8DFF000, что дизассемблер
# читает как `bl` — патч молча превращался бы в вызов.
THUMB_LDR_PC = 0xF000F8DF   # ldr.w pc, [pc, #0]
ARM_LDR_PC = 0xE51FF004     # ldr   pc, [pc, #-4]
THUMB_MOVW_PATCH = 10       # movw r12,#lo ; movt r12,#hi ; bx r12
THUMB_BX_R12 = 0x4760

# Почему у Thumb-патча две формы. `ldr.w pc, [pc, #imm]` считает адрес как
# Align(PC,4)+imm, а литерал обязан быть выровнен по слову. При entry%4==0
# хватает 8 байт (литерал по entry+4). При entry%4==2 литерал по entry+4 НЕ
# выровнен, и ближайшее выровненное место — entry+6, то есть 10 байт; тогда
# берётся форма без литерала вовсе: movw/movt/bx r12 (10 байт, выравнивания не
# требует). ARM-режим всегда 4-выровнен, там 8 байт.


def _arm_imm12(field):
    """ARM расширенный непосредственный: imm8, повёрнутый вправо на 2*rot."""
    imm8 = field & 0xFF
    rot = (field >> 8) & 0xF
    if rot == 0:
        return imm8
    return ((imm8 >> (2 * rot)) | ((imm8 << (32 - 2 * rot)) & 0xFFFFFFFF)) & 0xFFFFFFFF


def _thumb_mov_imm16(imm16, rd, movt):
    """Полуслова MOVW/MOVT Thumb-2 для (imm16, rd)."""
    i = (imm16 >> 11) & 1
    imm4 = (imm16 >> 12) & 0xF
    imm3 = (imm16 >> 8) & 7
    imm8 = imm16 & 0xFF
    hw1 = (0xF2C0 if movt else 0xF240) | (i << 10) | imm4
    hw2 = (imm3 << 12) | (rd << 8) | imm8
    return hw1, hw2


def arm_patch_size(addr):
    """Сколько байт занимает патч входа по адресу (как его отдал dlsym)."""
    if (addr & 1) and (addr & ~1) % 4 != 0:
        return THUMB_MOVW_PATCH
    return PATCH_SIZE_ARM


def arm_patch_bytes(addr, handler):
    """Байты патча входа для ARM32-цели, в порядке записи."""
    if addr & 1:
        even = addr & ~1
        if even % 4 == 0:
            return struct.pack("<II", THUMB_LDR_PC, handler)
        lo, hi = handler & 0xFFFF, (handler >> 16) & 0xFFFF
        w1, w2 = _thumb_mov_imm16(lo, 12, False)
        w3, w4 = _thumb_mov_imm16(hi, 12, True)
        return struct.pack("<HHHHH", w1, w2, w3, w4, THUMB_BX_R12)
    return struct.pack("<II", ARM_LDR_PC, handler)


def _thumb_branch_imm(hw1, hw2):
    """Знаковый imm для B.W/BL/BLX T4 по двум полусловам."""
    s = (hw1 >> 10) & 1
    j1 = (hw2 >> 13) & 1
    j2 = (hw2 >> 11) & 1
    i1 = 1 - (j1 ^ s)
    i2 = 1 - (j2 ^ s)
    imm = (s << 24) | (i1 << 23) | (i2 << 22) \
        | ((hw1 & 0x3FF) << 12) | ((hw2 & 0x7FF) << 1)
    if s:
        imm -= (1 << 25)
    return imm


def arm_plt_symbol(elf, stub):
    """Имя символа, на который ведёт .plt-заглушка ARM.

    Заглушка (.plt, ARM-режим, 16 байт на вход):
        add r12, pc, #imm       ; база = stub+8
        add r12, r12, #imm      ; (необязательно)
        ldr pc, [r12, #imm]!    ; pre-indexed: r12 += imm, загрузка оттуда
    Полученный адрес — слот .got.plt, а имя даёт .rel.plt (R_ARM_JUMP_SLOT).
    """
    w0, w1, w2 = elf.word(stub), elf.word(stub + 4), elf.word(stub + 8)
    if w0 is None or w1 is None or w2 is None:
        return None
    if (w0 & 0xFFFFF000) != 0xE28FC000:          # add r12, pc, #imm
        return None
    base = stub + 8 + _arm_imm12(w0 & 0xFFF)
    if (w1 & 0xFFFFF000) == 0xE28CC000:          # add r12, r12, #imm
        base += _arm_imm12(w1 & 0xFFF)
        ldr = w2
    else:
        ldr = w1
    if (ldr & 0xFFFFF000) != 0xE5BCF000:         # ldr pc, [r12, #imm]!
        return None
    return elf.rela_plt.get(base + (ldr & 0xFFF))


def _thumb_long_thunk_target(elf, addr, size):
    """__ThumbV7PILongThunk_*: 12 байт, movw/movt r12 ; add r12,pc ; bx r12."""
    if size != 12:
        return None
    hw = [elf.half(addr + 2 * i) for i in range(6)]
    if any(h is None for h in hw):
        return None
    if (hw[0] & 0xFBF0) != 0xF240:               # movw r12, #imm16 (T3)
        return None
    if (hw[2] & 0xFBF0) != 0xF2C0:               # movt r12, #imm16 (T1)
        return None
    if hw[4] != 0x44FC or hw[5] != 0x4760:       # add r12, pc ; bx r12
        return None
    lo = ((hw[0] & 0xF) << 12) | (((hw[0] >> 10) & 1) << 11) \
        | (((hw[1] >> 12) & 7) << 8) | (hw[1] & 0xFF)
    hi = ((hw[2] & 0xF) << 12) | (((hw[2] >> 10) & 1) << 11) \
        | (((hw[3] >> 12) & 7) << 8) | (hw[3] & 0xFF)
    pc = (addr + 12) & ~3                        # add стоит по addr+8
    return pc + ((hi << 16) | lo)


def _arm_long_thunk_target(elf, addr, size):
    """__ARMV7PILongThunk_*: 16 байт, movw/movt r12 ; add r12,r12,pc ; bx r12."""
    if size != 16:
        return None
    w = [elf.word(addr + 4 * i) for i in range(4)]
    if any(x is None for x in w):
        return None
    if (w[0] & 0xFFF0F000) != 0xE300C000:        # movw r12, #imm16
        return None
    if (w[1] & 0xFFF0F000) != 0xE340C000:        # movt r12, #imm16
        return None
    if w[2] != 0xE08CC00F:                       # add r12, r12, pc
        return None
    if w[3] != 0xE12FFF1C:                       # bx r12
        return None
    lo = ((w[0] >> 16) & 0xF) << 12 | (w[0] & 0xFFF)
    hi = ((w[1] >> 16) & 0xF) << 12 | (w[1] & 0xFFF)
    return (addr + 8) + 8 + ((hi << 16) | lo)    # pc = адрес add + 8


def arm_long_thunk_target(elf, addr, size):
    """Длинный переходник ARM32 (Thumb или ARM) -> адрес .plt-заглушки."""
    return (_thumb_long_thunk_target(elf, addr, size)
            if addr & 1 else _arm_long_thunk_target(elf, addr, size))


def thumb_branch_target(elf, addr, size):
    """Цель хвостового БЕЗУСЛОВНОГО перехода Thumb-функции (b.w или b.n)."""
    if not size or size < 2:
        return None
    last = elf.half(addr + size - 2)
    if last is None:
        return None
    if (last & 0xF800) == 0xE000:                # b.n T2 (2 байта)
        imm = last & 0x7FF
        if imm & 0x400:
            imm -= 0x800
        return addr + size + 2 + imm * 2
    if size < 4:
        return None
    hw1 = elf.half(addr + size - 4)
    if hw1 is None or (hw1 & 0xF800) != 0xF000 or (last & 0xD000) != 0x9000:
        return None                              # b.w T4 (4 байта)
    return addr + size + _thumb_branch_imm(hw1, last)


def arm_branch_target(elf, addr, size):
    """Цель хвостового B (ARM, cond=AL) в конце ARM-функции."""
    if not size or size < 4:
        return None
    w = elf.word(addr + size - 4)
    if w is None or (w & 0xFF000000) != 0xEA000000:
        return None
    off = w & 0xFFFFFF
    if off & 0x800000:
        off -= 0x1000000
    return addr + size + 4 + off * 4


def arm_mode_thumb(elf, addr):
    """Thumb ли цель. Бит режима берём из СИМВОЛА, а не из адреса.

    Цель ветвления всегда чётна — младший бит по дороге теряется, — поэтому по
    адресу режим не восстановить. Зато st_value функции его хранит: у Thumb он
    нечётен. Так различаются __ThumbV7PILongThunk_* (Thumb) и
    __ARMV7PILongThunk_* (ARM), живущие в одной секции. Нет символа — считаем
    ARM: там ошибка дешевле, чем принять ARM-код за Thumb (у `bx r12` второе
    полуслово 0xE12F попадает в диапазон b.n и дало бы ложный хвост).
    """
    even = addr & ~1
    s = elf.func_at(even) or elf.func_at(even | 1)
    if s is not None:
        return bool(s.value & 1)
    return bool(addr & 1)


def arm_tail_target(elf, addr, size):
    """Хвостовой переход функции ARM32: (вид, цель).

    Переходником считается НЕ любой хвостовой переход, а ровно те формы, что
    порождает bionic:
      * Thumb-обёртка, кончающаяся `b.w`/`b.n` на __ThumbV7PILongThunk_<имя>;
      * длинный переходник __ThumbV7PILongThunk_* (movw/movt/add pc/bx r12);
      * длинный переходник __ARMV7PILongThunk_* (ARM-режим).

    Хвостовой `b` в ARM-коде переходником НЕ считается намеренно: им кончается
    каждый сисколл-стаб bionic (`b __set_errno_internal`, 32 байта), а стаб —
    это и есть корень, который надо патчить. Правило «коротка и кончается
    переходом» (как на AArch64) приняло бы все четыре стаба за переходники и
    оставило образ без корней вообще.
    """
    even = addr & ~1
    if arm_mode_thumb(elf, even):
        tgt = thumb_branch_target(elf, even, size)
        if tgt is None:
            tgt = _thumb_long_thunk_target(elf, even, size)
    else:
        tgt = _arm_long_thunk_target(elf, even, size)
    if tgt is None:
        return "body", None
    return "thunk", tgt


def arm_call_targets(elf, addr, size):
    """Адреса вызовов (BL/BLX) в теле ARM32-функции.

    Режим берётся из символа (arm_mode_thumb), а не из бита адреса: тело — не
    цель ветвления, но функция может быть и Thumb, и ARM. В Thumb шаг 2 байта
    (инструкции 2 или 4 байта), в ARM — 4. Промах по выравниванию даёт пропуск,
    а не ложную цель: цель принимается только если разрешается в корень или .plt.
    """
    even = addr & ~1
    out = []
    if arm_mode_thumb(elf, even):
        for off in range(0, max(0, size - 3), 2):
            a = even + off
            hw1 = elf.half(a)
            hw2 = elf.half(a + 2)
            if hw1 is None or hw2 is None \
                    or (hw1 & 0xF800) != 0xF000 or (hw2 & 0xC000) != 0xC000:
                continue
            # BL и BLX считают базу по-разному: у BL это PC (a+4), у BLX PC
            # выравнивается по слову. Разница в 2 байта на 2-выровненной
            # инструкции, и без неё цель уезжает на 0xb2452 вместо 0xb2450 —
            # то есть .plt-заглушка __openat не опознаётся.
            base = (a + 4) if (hw2 & 0x1000) else ((a + 4) & ~3)
            out.append(base + _thumb_branch_imm(hw1, hw2))
        return out

    for off in range(0, max(0, size - 3), 4):
        a = even + off
        w = elf.word(a)
        if w is None:
            break
        if (w & 0xFF000000) == 0xEB000000:            # BL (ARM)
            o = w & 0xFFFFFF
            if o & 0x800000:
                o -= 0x1000000
            out.append(a + 8 + o * 4)
        elif (w & 0xFE000000) == 0xFA000000:          # BLX (immediate)
            o = w & 0xFFFFFF
            if o & 0x800000:
                o -= 0x1000000
            out.append(a + 8 + o * 4 + ((w >> 24) & 1) * 2)
    return out


def arm_entry_align(addr):
    """Начало без бита режима; для 8-байтного патча обязано быть кратно 4."""
    return (addr & ~1) % 4


# Patch only when the function is >= PATCH_SIZE; everything else is diagnostics.
# A thunk is short (<= THUNK_MAX) and ends in an unconditional branch — only those get unwrapped.

THUNK_MAX = 32


def classify_tail(elf, arch, addr, size):
    """Переходник ли функция: коротка и заканчивается безусловным переходом.

    У переходников bionic перестановки аргументов стоят в НАЧАЛЕ, а переход — в
    конце. Например creat (12 байт на AArch64):

        84f68: mov w2, w1          ; аргументы
        84f6c: mov w1, #0x241
        84f70: b   open@plt        ; хвостовой переход

    Поэтому проверять первую инструкцию бесполезно — она не переход. Ищем только
    B (0010 0110), не BL: вызов с возвратом — это обычное тело. На ARM32 форма
    своя (b.w / b.n, плюс длинный переходник `bx r12`) — см. arm_tail_target.

    Возвращает (вид, адрес_цели).
    """
    if arch == "arm":
        return arm_tail_target(elf, addr, size)
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


def is_thunk(elf, arch, addr, size):
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
    kind, _ = classify_tail(elf, arch, addr, size)
    return kind == "thunk"


def entry_addr(arch, value):
    """Исполняемое начало цели: у Thumb-функции бит 0 — это режим, не адрес."""
    return (value & ~1) if arch == "arm" else value


def need_size(arch, value):
    """Сколько байт нужно, чтобы поставить патч по этому адресу."""
    return arm_patch_size(value) if arch == "arm" else PATCH_SIZE


def align_ok(arch, value):
    """Хватает ли выравнивания под патч.

    AArch64 и ARM-режим — строго по 4. У Thumb обе формы доступны (8 байт при
    entry%4==0, 10 байт иначе), поэтому выравнивание не мешает никогда.
    """
    if arch == "arm":
        return (value & 1) != 0 or value % 4 == 0
    return value % 4 == 0


def func_at_any(elf, arch, addr):
    """Символ-функция по адресу, с учётом бита режима Thumb (st_value нечётен)."""
    addr = entry_addr(arch, addr)
    s = elf.func_at(addr)
    if s is None and arch == "arm":
        s = elf.func_at(addr | 1)
    return s


def size_at(elf, arch, addr):
    """Размер функции по адресу: st_size, иначе расстояние до следующего символа."""
    addr = entry_addr(arch, addr)
    s = func_at_any(elf, arch, addr)
    if s is not None and s.size:
        return s.size
    cands = [x.value for x in elf.symbols
             if x.type in (STT_FUNC, STT_GNU_IFUNC) and x.value > addr
             and (s is None or x.shndx == s.shndx)]
    return (min(cands) - addr) if cands else None


def iter_calls(elf, arch, addr, size):
    """Адреса вызовов (BL/BLX) в теле функции — с учётом ABI."""
    if arch == "arm":
        return arm_call_targets(elf, addr, size)
    out = []
    for off in range(0, size - 3, 4):
        w = elf.word(addr + off)
        if w is None:
            break
        if (w & 0xFC000000) != 0x94000000:      # BL only
            continue
        off26 = w & 0x03FFFFFF
        if off26 & 0x02000000:
            off26 -= 0x04000000
        out.append(addr + off + off26 * 4)
    return out


def plt_symbol(elf, arch, addr):
    """Имя символа для .plt-заглушки по адресу — с учётом ABI."""
    return arm_plt_symbol(elf, addr) if arch == "arm" else plt_stub_symbol(elf, addr)


def body_calls_root(elf, arch, addr, size, roots_by_addr, seen):
    """Ищет в теле функции вызов (bl) на уже пропатченный корень.

    Нужно для mkstemp и родни: они переходят на длинную mktemp_internal, а та
    внутри себя зовёт open@plt. Размотать это хвостовой цепочкой нельзя, поэтому
    тело просматривается целиком.

    Поиск транзитивный: bl может вести на функцию, которая сама не корень, но
    доходит до корня (open -> bl __openat, где __openat — форм-цель модуля, а
    open из HOOK_NAMES выведен). Тогда цепочка всё равно покрыта.
    """
    if not size:
        return None
    for tgt in iter_calls(elf, arch, addr, size):
        if tgt in roots_by_addr:
            return tgt
        if elf.in_plt(tgt):
            name = plt_symbol(elf, arch, tgt)
            if name is None:
                continue
            sym = next((s for s in elf.symbols if s.name == name and s.value), None)
            if sym is None:
                continue
            sv = entry_addr(arch, sym.value)
            if sv in roots_by_addr:
                return sv
            # Транзитивно: open@plt -> open -> bl __openat (корень формы).
            if sv not in seen:
                res, root, _ = chase(elf, arch, sv, roots_by_addr,
                                     0, [], seen | {addr})
                if res in ("root", "indirect"):
                    return root
    return None


def chase(elf, arch, addr, roots_by_addr, depth=0, trail=None, seen=None):
    """Идёт по цепочке хвостовых переходов до корня.

    Возвращает (kind, root_addr, trail); kind — root / indirect / uncovered /
    cycle / deep / unreadable.
    """
    trail = trail or []
    seen = seen or set()
    addr = entry_addr(arch, addr)
    if addr in roots_by_addr:
        return "root", addr, trail
    if addr in seen:
        return "cycle", None, trail
    if depth > 8:
        return "deep", None, trail
    seen.add(addr)

    size = size_at(elf, arch, addr)
    if size is None:
        return "unreadable", None, trail

    if size > THUNK_MAX:
        # Real function reached: nothing to unwind, but check for a call to a root.
        root = body_calls_root(elf, arch, addr, size, roots_by_addr, seen)
        if root is not None:
            return "indirect", root, trail
        return "uncovered", None, trail

    kind, tgt = classify_tail(elf, arch, addr, size)
    if kind != "thunk":
        # Короткое тело, не переходник: всё равно покрыто, если зовёт пропатченный
        # корень ближайшим bl. Так open/openat (снятые с модуля) покрываются через
        # ближайший bl __openat — а __openat теперь форм-цель модуля.
        root = body_calls_root(elf, arch, addr, size, roots_by_addr, seen)
        if root is not None:
            return "indirect", root, trail
        return "uncovered", None, trail

    if elf.in_plt(tgt):
        name = plt_symbol(elf, arch, tgt)
        if name is None:
            return "uncovered", None, trail + ["%s->.plt(неизв.)" % hex(addr)]
        sym = next((s for s in elf.symbols if s.name == name and s.value), None)
        if sym is None:
            return "uncovered", None, trail + ["%s->%s(нет адреса)" % (hex(addr), name)]
        return chase(elf, arch, sym.value, roots_by_addr, depth + 1,
                     trail + ["%s->%s@plt" % (hex(addr), name)], seen)

    label = func_at_any(elf, arch, tgt)
    shown = label.name if label else hex(tgt)
    return chase(elf, arch, tgt, roots_by_addr, depth + 1,
                 trail + ["%s->%s" % (hex(addr), shown)], seen)


def analyse(elf, arch):
    """Возвращает (записи по именам, набор корней, есть ли непокрытая цель).

    arch — "a64", "arm" или None (машина не из поддерживаемых). План патча и
    ширина патча зависят от ABI, поэтому arch идёт через весь разбор.
    """
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
        if arch is None:
            role = "foreign"
        elif eff is None:
            role = "unknown"
        elif eff < need_size(arch, s.value):
            role = "short"
        elif is_thunk(elf, arch, s.value, eff):
            role = "thunk"
        elif name in MODULE_HOOKS:
            role = "root"
        else:
            # Тело не из kHooks: модуль его не патчит, но обязан покрыть — это
            # проверяется через вызов корня (chase -> body_calls_root). Так на
            # ARM32 покрываются rename (-> renameat2@plt) и link (-> linkat@plt).
            role = "body"
        entries[name] = {"sym": s, "eff": eff, "next": nxt.name if nxt else None,
                         "gap": (nxt.value - s.value) if nxt else None,
                         "role": role}

    roots_by_addr = {entry_addr(arch, e["sym"].value) for e in entries.values()
                     if e["role"] == "root" and e["sym"]}
    addr_name = {}
    for name, e in entries.items():
        if e["role"] == "root" and e["sym"]:
            addr_name.setdefault(entry_addr(arch, e["sym"].value), name)

    bad = False

    # Форм-цель модуля: стаб сисколла __openat. Он ЛОКАЛЬНЫЙ (не в .dynsym, поэтому
    # не в HOOK_NAMES), но модуль патчит именно его вместо снятых open/openat.
    # Находим по имени в .symtab и трактуем как корень, если он патчпригоден.
    stub = next((s for s in elf.symbols
                 if s.name == "__openat" and s.value
                 and s.type in (STT_FUNC, STT_GNU_IFUNC)), None)
    stub_row = None
    if stub is not None:
        snxt = next_after(stub)
        seff = stub.size or ((snxt.value - stub.value) if snxt else None)
        saddr = entry_addr(arch, stub.value)
        sneed = need_size(arch, stub.value)
        if arch is None:
            stub_verdict = "skip"
            stub_note = "ABI не поддерживается — правка входа не реализована"
        elif seff is None:
            stub_verdict = "BAD"
            stub_note = "размер неизвестен — патч недопустим"
            bad = True
        elif seff < sneed:
            stub_verdict = "BAD"
            stub_note = "короче %d байт — патч затрёт соседнюю" % sneed
            bad = True
        elif not align_ok(arch, stub.value):
            stub_verdict = "BAD"
            stub_note = "адрес не выровнен — патч не встанет"
            bad = True
        elif is_thunk(elf, arch, stub.value, seff):
            stub_verdict = "BAD"
            stub_note = "переходник — патчить нельзя"
            bad = True
        else:
            stub_verdict = "ok"
            stub_note = ("патчится по форме (%s байт), заменяет open/openat" % seff)
        roots_by_addr.add(saddr)
        addr_name.setdefault(saddr, "__openat")
        stub_row = {"name": "__openat", "value": stub.value, "size": stub.size,
                    "eff": seff, "gap": (snxt.value - saddr) if snxt else None,
                    "next": snxt.name if snxt else None,
                    "align": saddr % 4 if arch == "arm" else stub.value % 4,
                    "type": STT_NAMES.get(stub.type, str(stub.type)),
                    "bind": STB_NAMES.get(stub.bind, str(stub.bind)),
                    "role": "root", "verdict": stub_verdict, "note": stub_note}

    rows = []
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
               "role": e["role"],
               "align": entry_addr(arch, s.value) % 4}
        notes = []

        if arch is None:
            notes.append("ABI не поддерживается — правка входа не реализована")
            row["verdict"] = "skip"
            rows.append(_finish(row, notes))
            # Every target is uncovered here, not merely absent: patch_entry()
            # is compiled out for this ABI (src/hook_libc.cpp returns false), so
            # nothing on this image can be patched by this module. Reporting
            # that as a pass would be a false green — the per-line note is not
            # enough, because the summary and the exit code are what a script
            # reads. See report() for the same statement at file level.
            bad = True
            continue

        if s.type == STT_GNU_IFUNC:
            notes.append("IFUNC: st_value может указывать на резолвер")
            row["verdict"] = "BAD"
            bad = True
        if not align_ok(arch, s.value):
            notes.append("адрес не выровнен по 4 байта — патч не встанет")
            row["verdict"] = "BAD"
            bad = True
        if s.bind != 1:
            notes.append("не GLOBAL (%s)" % STB_NAMES.get(s.bind, s.bind))

        if e["role"] == "root":
            row.setdefault("verdict", "ok")
            if row["verdict"] != "BAD":
                row["verdict"] = "ok"
            mode = ""
            if arch == "arm":
                mode = " [Thumb-2]" if (s.value & 1) else " [ARM]"
            notes.append("патчится (%s байт, патч %d байт)%s"
                         % (e["eff"], need_size(arch, s.value), mode))
        elif e["role"] == "unknown":
            notes.append("размер неизвестен — патч недопустим")
            row["verdict"] = "BAD"
            bad = True
        elif e["role"] in ("short", "thunk", "body"):
            # Ни один не патчится: все три покрываются тем, что вызов доходит до
            # пропатченного корня сам — коротка и переходник хвостом, тело через bl.
            kind, _ = classify_tail(elf, arch, s.value, e["eff"])
            if e["role"] == "short" and kind != "thunk":
                notes.append("короче %d байт и не переходник — патч затрёт соседнюю"
                             % need_size(arch, s.value))
                row["verdict"] = "BAD"
                bad = True
            else:
                res, root, trail = chase(elf, arch, s.value, roots_by_addr)
                if res in ("root", "indirect"):
                    rname = addr_name.get(root, hex(root))
                    row["verdict"] = "ok"
                    how = "через" if res == "root" else "косвенно через"
                    why = {"thunk": "переходник", "short": "коротка",
                           "body": "тело"}[e["role"]]
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

    if stub_row is not None:
        rows.append(_finish(stub_row, [stub_row["note"]]))
    elif arch is not None:
        # __openat не найден — модуль не сможет перехватить open-семейство
        # (нет корня для снятых open/openat).
        print("  ВНИМАНИЕ: __openat не найден — цель формы отсутствует",
              file=sys.stderr)
        bad = True

    collisions = []
    addrs = sorted(roots_by_addr)
    for a, b in zip(addrs, addrs[1:]):
        # Ширина патча у ARM32 зависит от выравнивания цели, поэтому берём
        # максимум из двух соседей, а не одну константу.
        need = max(need_size(arch, a), need_size(arch, b)) if arch else PATCH_SIZE
        if b - a < need:
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
    # Строка инструкции — «<hex-адрес>: ...». Прежний фильтр требовал двоеточие,
    # сразу за которым табуляция, а llvm-objdump печатает после двоеточия пробел,
    # поэтому отбиралась одна шапка «<путь>: file format …», а сами инструкции
    # молча отбрасывались — проверка кодирования ничего не показывала.
    return [ln.rstrip() for ln in r.stdout.splitlines()
            if re.match(r"^\s*[0-9a-fA-F]+:\s", ln)]


def patch_and_show(elf, arch, rows, patch_copy):
    """Пишет патч в КОПИЮ файла и возвращает список сделанных правок."""
    data = bytearray(elf.data)
    patched = []
    for row in rows:
        if row.get("verdict") != "ok" or "value" not in row:
            continue
        if row["role"] != "root":
            continue

        if arch == "arm":
            raw = row["value"]
            addr = raw & ~1
            off = elf.vaddr_to_offset(addr)
            if off is None:
                continue
            n = arm_patch_size(raw)
            before = bytes(data[off:off + n])
            blob = arm_patch_bytes(raw, addr + n)   # литерал/обработчик-маркер
            data[off:off + n] = blob
            patched.append({"name": row["name"], "addr": addr,
                            "before": before.hex(), "after": blob.hex()})
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


def report(path, elf, arch, rows, roots, addr_name, collisions, bad, patched, tool,
           verbose, disasm_path=None):
    pdesc = "8/10" if arch == "arm" else str(PATCH_SIZE)
    print("=" * 78)
    print("файл:   %s" % path)
    print("машина: %s   класс: %s   тип: %s   патч: %s байт"
          % (elf.machine_name(), "ELF64" if elf.is64 else "ELF32",
             elf.type_name(), pdesc))
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

    if arch is None:
        # A file-level statement, not a per-line note: this image cannot be
        # patched by this module at all, so "ok=0" here is a finding, and the
        # exit code says so (see analyse()).
        print()
        print("ЭТОТ ОБРАЗ — НЕ ПОДДЕРЖИВАЕМАЯ ABI (%s): правка входа в libc для"
              % elf.machine_name())
        print("неё не реализована — patch_entry() в src/hook_libc.cpp для неё")
        print("возвращает false, то есть ни одна из %d целей не патчится. Модуль"
              % len(rows))
        print("собирается только под arm64-v8a и armeabi-v7a, так что это не")
        print("ошибка образа, а отсутствие ABI в покрытии.")

    if collisions:
        print("НАЛОЖЕНИЕ патчей (ближе ширины патча):")
        for a, b, d in collisions:
            print("    %s и %s — %d байт" % (a, b, d))

    triple = triple_for(elf)
    if verbose and patched and triple and disasm_path:
        print()
        print("--- проверка кодирования патча (в копии) ---")
        for p in patched:
            n = len(p["before"]) // 2          # ширина патча у ARM32 своя на цель
            print("  %s @ 0x%x" % (p["name"], p["addr"]))
            print("    до:    %s" % " ".join(p["before"][i:i + 8]
                                            for i in range(0, n * 2, 8)))
            print("    после: %s" % " ".join(p["after"][i:i + 8]
                                            for i in range(0, n * 2, 8)))
            for ln in disasm(tool, disasm_path, p["addr"], triple, max(1, n // 4)):
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

            arch = ("a64" if elf.machine == MACHINE_AARCH64
                    else "arm" if elf.machine == MACHINE_ARM else None)
            rows, roots, addr_name, collisions, bad = analyse(elf, arch)
            any_bad = any_bad or bad

            patched = []
            disasm_path = None
            if not args.quiet:
                if tmpdir is None:
                    tmpdir = tempfile.mkdtemp(prefix="hookcheck.")
                disasm_path = os.path.join(tmpdir,
                                           os.path.basename(path) + ".patched")
                patched = patch_and_show(elf, arch, rows, disasm_path)
                if i == 0 and args.patch_copy:
                    shutil.copyfile(disasm_path, args.patch_copy)
                    print("пропатченная копия: %s" % args.patch_copy)

            report(path, elf, arch, rows, roots, addr_name, collisions, bad,
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
