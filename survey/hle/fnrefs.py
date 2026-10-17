#!/usr/bin/env python3
# fnrefs.py <lo> <hi>: code that materialises (lis + addi/ori) the address of a
# function in [lo, hi), and data words in the image that hold one.
import pickle, re, struct, sys, collections
sys.argv += []
with open('/work/hle/xref.pkl', 'rb') as f:
    funcs, insns, calls, callees = pickle.load(f)
starts = set(funcs)
lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
rx_lis = re.compile(r'^lis (r\d+),(-?\d+)$')
rx_addi = re.compile(r'^(?:addi|ori) (r\d+),(r\d+),(-?\d+)$')
refs = collections.defaultdict(list)
for fn in funcs:
    known = {}
    for a, t in insns[fn]:
        t = t.strip()
        m = rx_lis.match(t)
        if m:
            known[m.group(1)] = (int(m.group(2)) << 16) & 0xFFFFFFFF
            continue
        m = rx_addi.match(t)
        if m and m.group(2) in known:
            v = (known[m.group(2)] + int(m.group(3))) & 0xFFFFFFFF if t.startswith('addi') else known[m.group(2)] | int(m.group(3))
            if v in starts and lo <= v < hi:
                refs[v].append((fn, a, 'code'))
            continue
BASE = 0x82000000
d = open('/work/hle/image.bin', 'rb').read()
for o in range(0, min(len(d), 0xCD0000) - 3, 4):
    if BASE + o >= 0x820E0000 and BASE + o < 0x828D0000:
        continue
    v = struct.unpack('>I', d[o:o + 4])[0]
    if lo <= v < hi and v in starts:
        refs[v].append((None, BASE + o, 'data'))
for v in sorted(refs):
    print(f'sub_{v:08X}: ' + ' '.join(f'{"sub_%08X@" % fn if fn else "data@"}{a:08X}' for fn, a, k in refs[v]))
