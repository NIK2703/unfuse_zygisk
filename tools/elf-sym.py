#!/usr/bin/env python3
"""Locate a dynamically imported symbol in an ELF64 shared object / PIE.

Prints, for each requested symbol:
  * its index in .dynsym
  * the .rela.plt relocation that fills its GOT slot -> the GOT slot's
    *virtual address* (r_offset) and its *file offset*
  * the offset of the PLT stub that jumps through that GOT slot (for AArch64
    the stub is reached via .rela.plt order: plt[i] -> plt_base + i*16)

Usage: elf-sym.py <elf> <symbol> [<symbol> ...]
"""
import struct
import sys


def parse(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF':
        raise SystemExit('not an ELF')
    if d[4] != 2:
        raise SystemExit('only ELF64 supported (class=%d)' % d[4])
    e_shoff = struct.unpack_from('<Q', d, 0x28)[0]
    e_shentsize = struct.unpack_from('<H', d, 0x3a)[0]
    e_shnum = struct.unpack_from('<H', d, 0x3c)[0]
    e_shstrndx = struct.unpack_from('<H', d, 0x3e)[0]
    shs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        (name, typ, flags, addr, off, size, link, info, align,
         entsize) = struct.unpack_from('<IIQQQQIIQQ', d, o)
        shs.append(dict(i=i, name=name, typ=typ, flags=flags, addr=addr,
                        off=off, size=size, link=link, info=info,
                        entsize=entsize))
    shstr = shs[e_shstrndx]

    def nm(x):
        s = shstr['off'] + x
        e = d.index(b'\0', s)
        return d[s:e].decode()

    for s in shs:
        s['n'] = nm(s['name'])
    return d, shs


def find(shs, name):
    for s in shs:
        if s['n'] == name:
            return s
    return None


def main():
    path = sys.argv[1]
    want = sys.argv[2:]
    d, shs = parse(path)

    dynsym = find(shs, '.dynsym')
    dynstr = find(shs, '.dynstr')
    relaplt = find(shs, '.rela.plt')
    rela_dyn = find(shs, '.rela.dyn')
    plt = find(shs, '.plt')

    if not dynsym or not dynstr:
        raise SystemExit('no .dynsym/.dynstr')

    def strtab(sh, x):
        s = sh['off'] + x
        e = d.index(b'\0', s)
        return d[s:e].decode()

    nent = dynsym['size'] // 24
    syms = {}
    for i in range(nent):
        o = dynsym['off'] + i * 24
        st_name, st_info, st_other, st_shndx, st_value, st_size = \
            struct.unpack_from('<IBBHQQ', d, o)
        name = strtab(dynstr, st_name) if st_name else ''
        syms[i] = dict(name=name, value=st_value, shndx=st_shndx,
                       size=st_size, info=st_info)

    # map name -> dynsym index (undefined symbols: st_shndx == 0)
    by_name = {}
    for i, s in syms.items():
        if s['name']:
            by_name.setdefault(s['name'], []).append(i)

    # relocation tables
    relocs = []
    for sh, kind in ((relaplt, 'JMP_SLOT'), (rela_dyn, 'GLOB_DAT')):
        if not sh:
            continue
        cnt = sh['size'] // 24
        for i in range(cnt):
            o = sh['off'] + i * 24
            r_offset, r_info, r_addend = struct.unpack_from('<QQq', d, o)
            sym = r_info >> 32
            rtype = r_info & 0xffffffff
            relocs.append(dict(off=r_offset, sym=sym, type=rtype,
                               kind=kind, idx=i))

    print('== %s ==' % path)
    print('.dynsym %d syms, .rela.plt %s, .rela.dyn %s'
          % (nent,
             ('%d' % (relaplt['size'] // 24)) if relaplt else '-',
             ('%d' % (rela_dyn['size'] // 24)) if rela_dyn else '-'))

    for w in want:
        idxs = by_name.get(w)
        print('\n-- %s --' % w)
        if not idxs:
            print('  NOT in .dynsym')
            continue
        for i in idxs:
            s = syms[i]
            undef = 'UNDEF' if s['shndx'] == 0 else 'DEF(shndx=%d)' % s['shndx']
            print('  dynsym[%d] value=0x%x %s' % (i, s['value'], undef))
            for r in relocs:
                if r['sym'] != i:
                    continue
                foff = None
                for sh in shs:
                    if sh['addr'] <= r['off'] < sh['addr'] + sh['size'] \
                            and sh['typ'] != 8:  # skip NOBITS
                        foff = sh['off'] + (r['off'] - sh['addr'])
                        secn = sh['n']
                        break
                else:
                    secn = '?'
                print('    reloc %-9s type=%d off(VA)=0x%x sec=%s file=0x%x'
                      % (r['kind'], r['type'], r['off'], secn,
                         foff if foff is not None else -1))
                if r['kind'] == 'JMP_SLOT' and plt:
                    # AArch64 PLT: entry 0 = PLT0, stub k at plt.addr + 16*(k+1)
                    stub = plt['addr'] + 16 * (r['idx'] + 1)
                    print('    plt stub[%d] VA=0x%x file=0x%x'
                          % (r['idx'], stub,
                             plt['off'] + (stub - plt['addr'])))
                if foff is not None:
                    cur = struct.unpack_from('<Q', d, foff)[0]
                    print('    current GOT content = 0x%x' % cur)


main()
