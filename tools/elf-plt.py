#!/usr/bin/env python3
"""Analyze how a given ELF64 (AArch64) reaches a dynamically imported libc
symbol, and prove that the PLT stub we would patch really is that symbol's.

For the requested symbol it reports:
  1. .dynsym entry (must be UNDEF)
  2. .rela.plt JMP_SLOT -> GOT slot VA
  3. the PLT stub VA (from the relocation index)
  4. a *decoded* check of the stub: adrp+ldr must land exactly on the GOT slot
  5. every call site in .text (bl / b) that targets the stub
  6. PT_GNU_RELRO / DF_BIND_NOW / DF_1_NOW state -> whether .got.plt is
     writable at runtime

Usage: elf-plt.py <elf> <symbol>
"""
import struct
import sys

A64 = {}


def sxt(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m


def decode_stub(d, off, expect_got):
    """Decode adrp/ldr/add/br and return (target_va_of_ldr, ok)."""
    w0, w1, w2, w3 = struct.unpack_from('<IIII', d, off)
    ok = True
    if (w0 & 0x9f000000) != 0x90000000:
        ok = False
    if (w1 & 0xffc00000) != 0xf9400000:
        ok = False
    if (w2 & 0xffc00000) != 0x91000000:
        ok = False
    if w3 != 0xd61f0220:
        ok = False
    pc = off  # file offset == VA in the first LOAD segment
    # adrp x16, #imm
    immlo = (w0 >> 29) & 3
    immhi = (w0 >> 5) & 0x7ffff
    imm = sxt((immhi << 2) | immlo, 21) << 12
    page = (pc & ~0xfff) + imm
    # ldr x17, [x16, #imm12*8]
    imm12 = (w1 >> 10) & 0xfff
    ldr_va = page + imm12 * 8
    return dict(ok=ok, adrp_page=page, ldr_va=ldr_va,
                add_imm=(w2 >> 10) & 0xfff,
                ldr_matches=(ldr_va == expect_got), raw=(w0, w1, w2, w3))


def main():
    path, sym = sys.argv[1], sys.argv[2]
    d = open(path, 'rb').read()
    e_shoff = struct.unpack_from('<Q', d, 0x28)[0]
    e_phoff = struct.unpack_from('<Q', d, 0x20)[0]
    e_phentsize = struct.unpack_from('<H', d, 0x36)[0]
    e_phnum = struct.unpack_from('<H', d, 0x38)[0]
    e_shnum = struct.unpack_from('<H', d, 0x3c)[0]
    e_shstrndx = struct.unpack_from('<H', d, 0x3e)[0]
    e_shentsize = struct.unpack_from('<H', d, 0x3a)[0]

    shs = []
    for i in range(e_shnum):
        f = struct.unpack_from('<IIQQQQIIQQ', d, e_shoff + i * e_shentsize)
        shs.append(dict(name=f[0], typ=f[1], flags=f[2], addr=f[3],
                        off=f[4], size=f[5], link=f[6], info=f[7],
                        entsize=f[9]))
    shstr = shs[e_shstrndx]

    def nm(x):
        s = shstr['off'] + x
        return d[s:d.index(b'\0', s)].decode()

    for s in shs:
        s['n'] = nm(s['name'])

    def sh(name):
        for s in shs:
            if s['n'] == name:
                return s

    def va2off(va):
        for s in shs:
            if s['typ'] != 8 and s['addr'] <= va < s['addr'] + s['size']:
                return s['off'] + (va - s['addr']), s['n']
        return None, None

    dynsym, dynstr = sh('.dynsym'), sh('.dynstr')
    relaplt, plt = sh('.rela.plt'), sh('.plt')

    def strtab(s, x):
        p = s['off'] + x
        return d[p:d.index(b'\0', p)].decode()

    dyn = sh('.dynamic')
    tags = {}
    if dyn:
        for i in range(dyn['size'] // 16):
            t, v = struct.unpack_from('<Qq', d, dyn['off'] + i * 16)
            tags.setdefault(t, []).append(v)
    DF_BIND_NOW = 0x8
    DF_1_NOW = 0x1
    flags = tags.get(0x1e, [0])[0]          # DT_FLAGS
    flags1 = tags.get(0x6ffffffb, [0])[0]   # DT_FLAGS_1
    print('DT_FLAGS=0x%x (BIND_NOW=%s)  DT_FLAGS_1=0x%x (NOW=%s)'
          % (flags, bool(flags & DF_BIND_NOW), flags1, bool(flags1 & DF_1_NOW)))
    print('FLAGS_1 0x6ffffffb present:', 0x6ffffffb in tags)

    relro = None
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, \
            p_align = struct.unpack_from('<IIQQQQQQ', d, o)
        if p_type == 0x6474e552:  # PT_GNU_RELRO
            relro = (p_vaddr, p_memsz)
    print('PT_GNU_RELRO:', ('VA=0x%x size=0x%x' % relro) if relro else 'none')

    idx = None
    for i in range(dynsym['size'] // 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = \
            struct.unpack_from('<IBBHQQ', d, dynsym['off'] + i * 24)
        if st_name and strtab(dynstr, st_name) == sym:
            idx = i
            print('dynsym[%d] %s shndx=%d value=0x%x'
                  % (i, sym, st_shndx, st_value))
            break
    if idx is None:
        raise SystemExit('symbol %r not in .dynsym' % sym)

    slot_va = slot_idx = None
    for i in range(relaplt['size'] // 24):
        r_offset, r_info, r_addend = struct.unpack_from('<QQq', d,
                                                        relaplt['off'] + i * 24)
        if (r_info >> 32) == idx:
            slot_va, slot_idx = r_offset, i
            print('.rela.plt[%d] JMP_SLOT type=%d -> GOT VA=0x%x'
                  % (i, r_info & 0xffffffff, r_offset))
    if slot_va is None:
        print('no .rela.plt entry (symbol is not lazily called)')
        return

    stub_va = plt['addr'] + 16 * (slot_idx + 1)
    stub_off = plt['off'] + (stub_va - plt['addr'])
    print('.plt base=0x%x entry0=PLT0  stub#%d VA=0x%x file=0x%x'
          % (plt['addr'], slot_idx, stub_va, stub_off))

    info = decode_stub(d, stub_off, slot_va)
    print('  stub decode: adrp page=0x%x  ldr -> 0x%x  (expect GOT 0x%x)'
          % (info['adrp_page'], info['ldr_va'], slot_va))
    print('  stub shape ok=%s  ldr lands on GOT slot=%s  raw=%s'
          % (info['ok'], info['ldr_matches'],
             ' '.join('%08x' % w for w in info['raw'])))

    if relro:
        lo, hi = relro[0], relro[0] + relro[1]
        inrelro = lo <= slot_va < hi
        print('  GOT slot inside RELRO: %s  -> %s'
              % (inrelro,
                 'read-only after relocation (mprotect needed)'
                 if (inrelro and (flags & DF_BIND_NOW or flags1 & DF_1_NOW))
                 else 'writable'))
    else:
        print('  GOT slot writable (no RELRO)')

    text = sh('.text')
    hits = []
    for off in range(text['off'], text['off'] + text['size'], 4):
        w = struct.unpack_from('<I', d, off)[0]
        if (w & 0xfc000000) in (0x94000000, 0x14000000):  # bl / b
            tgt = (off + sxt(w & 0x3ffffff, 26) * 4)
            if tgt == stub_va:
                hits.append((off, 'bl' if (w & 0xfc000000) == 0x94000000
                             else 'b'))
    print('  call sites in .text targeting the stub: %d' % len(hits))
    for off, kind in hits:
        print('    %s at VA=0x%x (file 0x%x)' % (kind, off, off))

    for sname in ('.rela.dyn',):
        s = sh(sname)
        if not s:
            continue
        n = 0
        for i in range(s['size'] // 24):
            r_offset, r_info, r_addend = struct.unpack_from('<QQq', d,
                                                            s['off'] + i * 24)
            if (r_info >> 32) == idx:
                n += 1
                print('  %s[%d] type=%d -> VA=0x%x (address-taken use!)'
                      % (sname, i, r_info & 0xffffffff, r_offset))
        if n == 0:
            print('  no %s entry -> symbol is never address-taken' % sname)


main()
