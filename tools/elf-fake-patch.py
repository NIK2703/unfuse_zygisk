#!/usr/bin/env python3
"""Сделать копию ELF, в которой трамплин уже пропатчен (mov w0,#0; ret).

Нужен, чтобы проверить идемпотентность vold-noacl офлайн, не трогая
/system/bin/vold на устройстве.

    python3 tools/elf-fake-patch.py device/bin/vold /tmp/vold.patched 0xfa5d0
"""
import struct
import sys


def va_to_off(data, va):
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_flags = struct.unpack_from("<II", data, off)
        if p_type != 1:  # PT_LOAD
            continue
        p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from(
            "<QQQQ", data, off + 8)
        if p_vaddr <= va < p_vaddr + p_filesz:
            return p_offset + (va - p_vaddr)
    raise SystemExit(f"VA {va:#x} не попала ни в один PT_LOAD")


def main():
    src, dst = sys.argv[1], sys.argv[2]
    stub_va = int(sys.argv[3], 0)
    data = bytearray(open(src, "rb").read())
    off = va_to_off(data, stub_va)
    old = bytes(data[off:off + 8])
    # mov w0, #0 ; ret
    new = struct.pack("<II", 0x52800000, 0xd65f03c0)
    data[off:off + 8] = new
    open(dst, "wb").write(data)
    print(f"{src} -> {dst}")
    print(f"  VA {stub_va:#x} = файловое смещение {off:#x}")
    print(f"  было:  {old.hex(' ')}")
    print(f"  стало: {new.hex(' ')}")


if __name__ == "__main__":
    main()
