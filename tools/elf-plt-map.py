#!/usr/bin/env python3
"""Build-independent locator for a dynamically imported symbol's PLT stub.

Instead of assuming `reloc_index -> plt_base + 16*(i+1)` (which is WRONG for
this linker: lld emits two header entries), this decodes *every* 16-byte entry
in .plt and matches it against the GOT slot taken from .rela.plt:

    adrp x16, page      ->  page
    ldr  x17, [x16, #k] ->  page + k*8   ==  relocation r_offset ?

That is self-validating: the match proves the stub belongs to the symbol, so
the patch never depends on a byte signature or on section sizes.

Usage: elf-plt-map.py <elf> [symbol ...]
"""
import struct
import sys


def sxt(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m


def load(path):
    d = open(path, 'rb').read()
    e_shoff = struct.unpack_from('<Q', d, 0x28)[0]
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
        p = shstr['off'] + x
        return d[p:d.index(b'\0', p)].decode()

    for s in shs:
        s['n'] = nm(s['name'])

    def sh(name):
        for s in shs:
            if s['n'] == name:
                return s
    return d, shs, sh


def stub_ldr_target(d, off):
    """Decode an AArch64 PLT entry; return the VA its `ldr x17,[x16,#imm]`
    reads, or None if the entry does not have the canonical shape."""
    if off + 16 > len(d):
        return None
    w0, w1, w2, w3 = struct.unpack_from('<IIII', d, off)
    if (w0 & 0x9f000000) != 0x90000000:      # adrp
        return None
    if (w1 & 0xffc00000) != 0xf9400000:      # ldr (imm, unsigned)
        return None
    if w3 != 0xd61f0220:                     # br x17
        return None
    immlo = (w0 >> 29) & 3
    immhi = (w0 >> 5) & 0x7ffff
    page = (off & ~0xfff) + (sxt((immhi << 2) | immlo, 21) << 12)
    return page + ((w1 >> 10) & 0xfff) * 8


def main():
    path = sys.argv[1]
    want = set(sys.argv[2:])
    d, shs, sh = load(path)

    dynsym, dynstr = sh('.dynsym'), sh('.dynstr')
    relaplt, plt = sh('.rela.plt'), sh('.plt')

    def strtab(s, x):
        p = s['off'] + x
        return d[p:d.index(b'\0', p)].decode()

    names = {}
    for i in range(dynsym['size'] // 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = \
            struct.unpack_from('<IBBHQQ', d, dynsym['off'] + i * 24)
        names[i] = (strtab(dynstr, st_name) if st_name else '', st_shndx)

    # r_offset -> (symidx, reloc index)
    jmpslots = {}
    for i in range(relaplt['size'] // 24):
        r_offset, r_info, r_addend = struct.unpack_from(
            '<QQq', d, relaplt['off'] + i * 24)
        jmpslots[r_offset] = (r_info >> 32, i, r_info & 0xffffffff)

    by_target = {}
    nstubs = 0
    base = plt['addr']
    for k in range(plt['size'] // 16):
        off = plt['off'] + 16 * k
        tgt = stub_ldr_target(d, off)
        if tgt is None:
            continue
        nstubs += 1
        va = base + 16 * k
        by_target.setdefault(tgt, []).append((k, va, off))

    print('== %s ==' % path)
    print('.plt base=0x%x size=0x%x  decoded entries=%d  JMP_SLOT relocs=%d'
          % (base, plt['size'], nstubs, len(jmpslots)))

    matched = sum(1 for t in jmpslots if t in by_target)
    print('JMP_SLOT slots matched to a decoded stub: %d / %d'
          % (matched, len(jmpslots)))
    dupes = {t: v for t, v in by_target.items() if len(v) > 1}
    print('ambiguous targets: %d' % len(dupes))

    # reverse map: symbol name -> stub
    name2stub = {}
    for t, (symidx, ridx, rtype) in jmpslots.items():
        sname, shndx = names.get(symidx, ('', -1))
        if t in by_target:
            k, va, off = by_target[t][0]
            name2stub.setdefault(sname, []).append(
                dict(slot=t, va=va, off=off, k=k, reloc=ridx, shndx=shndx))

    if not want:
        want = set(name2stub)

    for w in sorted(want):
        print('\n-- %s --' % w)
        ents = name2stub.get(w)
        if not ents:
            print('  no PLT stub found')
            continue
        for e in ents:
            print('  GOT slot VA=0x%x  ->  .plt entry #%d VA=0x%x file=0x%x'
                  '  (rela.plt[%d], dynsym shndx=%d)'
                  % (e['slot'], e['k'], e['va'], e['off'], e['reloc'],
                     e['shndx']))
            text = sh('.text')
            hits = []
            for off in range(text['off'], text['off'] + text['size'], 4):
                ww = struct.unpack_from('<I', d, off)[0]
                op = ww & 0xfc000000
                if op in (0x94000000, 0x14000000):
                    if off + sxt(ww & 0x3ffffff, 26) * 4 == e['va']:
                        hits.append((off, 'bl' if op == 0x94000000 else 'b'))
            print('  call sites in .text: %d' % len(hits))
            for off, kind in hits:
                print('    %s VA=0x%x' % (kind, off))
            rd = sh('.rela.dyn')
            n = 0
            if rd:
                for i in range(rd['size'] // 24):
                    r_offset, r_info, _ = struct.unpack_from(
                        '<QQq', d, rd['off'] + i * 24)
                    sname, _ = names.get(r_info >> 32, ('', -1))
                    if sname == w:
                        n += 1
                        print('    .rela.dyn type=%d -> VA=0x%x'
                              % (r_info & 0xffffffff, r_offset))
            if n == 0:
                print('  not address-taken (.rela.dyn clean)')


main()
