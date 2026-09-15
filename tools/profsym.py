#!/usr/bin/env python3
"""Symbolise a bennugd prof dump (raw little-endian uint32 pc,lr pairs)
against the ELF binary's symbol table: hottest functions, then the callers
of the hottest leaf functions (from lr).
  tools/profsym.py hps/out/bennugd prof.bin [top]"""
import bisect, struct, sys
from collections import Counter

def symbols(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'\x7fELF' and d[4] == 1, 'need a 32-bit ELF'
    shoff, = struct.unpack_from('<I', d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2e)
    secs = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize) for i in range(shnum)]
    syms = []
    for sec in secs:
        name, typ, flags, addr, off, size, link, info, align, entsize = sec
        if typ != 2:  # SHT_SYMTAB
            continue
        strtab = secs[link]
        for i in range(size // 16):
            st_name, st_value, st_size, st_info = struct.unpack_from('<IIIB', d, off + i * 16)
            if st_info & 0xf != 2 or st_size == 0:  # STT_FUNC
                continue
            end = d.index(b'\0', strtab[4] + st_name)
            syms.append((st_value & ~1, st_size, d[strtab[4] + st_name:end].decode()))
    syms.sort()
    return syms

def main():
    binary, dump = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    syms = symbols(binary)
    starts = [s[0] for s in syms]
    raw = open(dump, 'rb').read()
    words = struct.unpack('<%dI' % (len(raw) // 4), raw)
    def name(pc):
        i = bisect.bisect_right(starts, pc) - 1
        if i >= 0 and syms[i][0] <= pc < syms[i][0] + syms[i][1]:
            return syms[i][2]
        return '?? (%s)' % ('libc/unknown' if pc < 0x10000 or pc > 0x40000000 else hex(pc & ~0xfff))
    pairs = list(zip(words[0::2], words[1::2]))
    hits, callers = Counter(), {}
    for pc, lr in pairs:
        f = name(pc)
        hits[f] += 1
        callers.setdefault(f, Counter())[name(lr)] += 1
    total = len(pairs)
    print('%d samples (1 ms each) from over-budget frames' % total)
    for f, n in hits.most_common(top):
        print('%6.2f%%  %7d  %s' % (100.0 * n / total, n, f))
    print('\ncallers (by lr) of the top leaf-ish functions:')
    for f, n in hits.most_common(8):
        print('  %s:' % f)
        for c, m in callers[f].most_common(4):
            print('      %5.1f%%  %s' % (100.0 * m / n, c))

main()
