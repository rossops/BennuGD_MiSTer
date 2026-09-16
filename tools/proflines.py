#!/usr/bin/env python3
"""Attribute the samples of one function in a bennugd prof dump to source
lines, using the DWARF line table as printed by `objdump -d -l`.
  tools/proflines.py hps/out/bennugd prof.bin instance_go [top]
Needs Apple's or LLVM's objdump on PATH (both accept the ARM ELF)."""
import re, struct, subprocess, sys
from collections import Counter

def main():
    binary, dump, func = sys.argv[1], sys.argv[2], sys.argv[3]
    top = int(sys.argv[4]) if len(sys.argv) > 4 else 30
    # --disassemble-symbols stops at the first inline jump table, so take the
    # function's range from the symbol table and disassemble by address.
    nm = subprocess.run(['nm', '-S', '--defined-only', binary], capture_output=True, text=True, check=True).stdout
    start = size = None
    for l in nm.splitlines():
        p = l.split()
        if len(p) == 4 and p[3] == func:
            start, size = int(p[0], 16) & ~1, int(p[1], 16)
    if start is None:
        sys.exit('no symbol %s' % func)
    out = subprocess.run(['objdump', '-d', '-l', '--start-address=%d' % start,
                          '--stop-address=%d' % (start + size), binary],
                         capture_output=True, text=True, check=True).stdout
    line_of, cur, lo, hi = {}, '?', None, None
    for l in out.splitlines():
        m = re.match(r'^; (.*):(\d+)$', l)
        if m:
            cur = m.group(1).split('/')[-1] + ':' + m.group(2)
            continue
        m = re.match(r'^\s*([0-9a-f]+):\s', l)
        if m:
            a = int(m.group(1), 16)
            line_of[a] = cur
            lo = a if lo is None else min(lo, a)
            hi = a if hi is None else max(hi, a)
    raw = open(dump, 'rb').read()
    words = struct.unpack('<%dI' % (len(raw) // 4), raw)
    pcs = words[0::2]
    total = len(pcs)
    inside = [pc for pc in pcs if lo <= pc <= hi]
    hits = Counter(line_of.get(pc & ~3, '?') for pc in inside)
    print('%d samples, %d (%.1f%%) inside %s [%x-%x]' % (total, len(inside), 100.0 * len(inside) / max(1, total), func, lo, hi))
    for src, n in hits.most_common(top):
        print('%6.2f%%  %6d  %s' % (100.0 * n / max(1, len(inside)), n, src))

main()
