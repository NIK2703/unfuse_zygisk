#!/usr/bin/env python3
"""find-acl-writer.py — найти в vold код, который пишет default-ACL.

Зачем. vold собран stripped, символа SetDefaultAcl в нём нет. Но функция
единственная, кто пишет xattr system.posix_acl_default, и эта строка лежит в
.rodata ровно один раз. Значит функцию можно найти по ссылке на неё:
ищем пары adrp+add (или adrp+ldr), которые в сумме дают адрес строки.

Если такая ссылка одна — у нас одна точка правки. Если их несколько, значит
компилятор встроил (inlined) SetDefaultAcl в нескольких вызывающих, и правка
одного места ничего не даст — это важно знать заранее.

Использование:
    python3 tools/find-acl-writer.py device/bin/vold
"""

import struct
import sys

SHF_EXECINSTR = 0x4
SHT_STRTAB = 3


class Elf:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        if self.data[:4] != b"\x7fELF":
            raise SystemExit("не ELF: %s" % path)
        if self.data[4] != 2:
            raise SystemExit("ожидается ELF64")
        self.endian = "<" if self.data[5] == 1 else ">"
        e = self.endian
        (self.e_type, self.machine) = struct.unpack_from(e + "HH", self.data, 16)
        (self.e_shoff,) = struct.unpack_from(e + "Q", self.data, 0x28)
        (self.e_shentsize, self.e_shnum, self.e_shstrndx) = \
            struct.unpack_from(e + "HHH", self.data, 0x3A)
        self.sections = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            (name, stype, flags, addr, shoff, size) = \
                struct.unpack_from(e + "IIQQQQ", self.data, off)
            self.sections.append({"name_off": name, "type": stype, "flags": flags,
                                  "addr": addr, "offset": shoff, "size": size})
        # имена секций
        strtab = self.sections[self.e_shstrndx]
        base = strtab["offset"]
        for s in self.sections:
            end = self.data.index(b"\0", base + s["name_off"])
            s["name"] = self.data[base + s["name_off"]:end].decode("utf-8", "replace")

    def section_by_name(self, name):
        return next((s for s in self.sections if s["name"] == name), None)

    def offset_to_vaddr(self, off):
        for s in self.sections:
            if s["offset"] and s["offset"] <= off < s["offset"] + s["size"]:
                return s["addr"] + (off - s["offset"]), s["name"]
        return None, None

    def word(self, vaddr):
        off = self.vaddr_to_offset(vaddr)
        if off is None:
            return None
        return struct.unpack_from(self.endian + "I", self.data, off)[0]

    def vaddr_to_offset(self, vaddr):
        for s in self.sections:
            if s["addr"] and s["addr"] <= vaddr < s["addr"] + s["size"]:
                return s["offset"] + (vaddr - s["addr"])
        return None


def sext(value, bits):
    if value & (1 << (bits - 1)):
        value -= (1 << bits)
    return value


def adrp_target(insn, pc):
    """(reg, целевая страница) для ADRP, иначе None."""
    if (insn & 0x9F000000) != 0x90000000:
        return None
    rd = insn & 0x1F
    immlo = (insn >> 29) & 0x3
    immhi = (insn >> 5) & 0x7FFFF
    imm = sext((immhi << 2) | immlo, 21) << 12
    return rd, (pc & ~0xFFF) + imm


def add_imm(insn):
    """(rn, rd, imm12) для ADD Xd, Xn, #imm12 (64-бит, без сдвига)."""
    if (insn & 0xFFC00000) != 0x91000000:
        return None
    rn = (insn >> 5) & 0x1F
    rd = insn & 0x1F
    imm = (insn >> 10) & 0xFFF
    return rn, rd, imm


def ldr_imm(insn):
    """(rn, rt, imm12) для LDR Xt, [Xn, #imm12] (64-бит, scaled by 8)."""
    if (insn & 0xFFC00000) != 0xF9400000:
        return None
    rn = (insn >> 5) & 0x1F
    rt = insn & 0x1F
    imm = ((insn >> 10) & 0xFFF) * 8
    return rn, rt, imm


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "device/bin/vold"
    needle = b"system.posix_acl_default"
    elf = Elf(path)

    off = elf.data.find(needle)
    if off == -1:
        raise SystemExit("строка %s не найдена" % needle.decode())
    vaddr, secname = elf.offset_to_vaddr(off)
    print("строка %s" % needle.decode())
    print("  файловое смещение: 0x%x   секция: %s   vaddr: 0x%x" % (off, secname, vaddr))
    print()

    text = elf.section_by_name(".text")
    if text is None:
        raise SystemExit("нет .text")
    print(".text: vaddr=0x%x size=0x%x" % (text["addr"], text["size"]))
    print()

    # Сканируем: adrp, а затем в пределах следующих 6 инструкций add/ldr с тем же
    # регистром, дающие адрес строки.
    sites = []
    start, size = text["addr"], text["size"]
    for pc in range(start, start + size, 4):
        insn = elf.word(pc)
        if insn is None:
            continue
        a = adrp_target(insn, pc)
        if a is None:
            continue
        reg, page = a
        for k in range(1, 7):
            nxt_pc = pc + k * 4
            nxt = elf.word(nxt_pc)
            if nxt is None:
                break
            ai = add_imm(nxt)
            if ai and ai[0] == reg and page + ai[2] == vaddr:
                sites.append(("add", pc, nxt_pc, page + ai[2]))
                break
            li = ldr_imm(nxt)
            if li and li[0] == reg and page + li[2] == vaddr:
                sites.append(("ldr", pc, nxt_pc, page + li[2]))
                break

    print("ссылок на строку найдено: %d" % len(sites))
    for kind, pc, use, target in sites:
        print("  %s: adrp @ 0x%x, %s @ 0x%x -> 0x%x" % (kind, pc, kind, use, target))
    return 0 if sites else 1


if __name__ == "__main__":
    sys.exit(main())
