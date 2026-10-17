#!/usr/bin/env python3
# img.py: helpers over /work/hle/image.bin (the xex's image at 0x82000000).
#   img.py sections
#   img.py ptrs <func addr>...       where the image holds each address (function pointer tables)
#   img.py tables [min run]           runs of >= N consecutive code pointers in data
#   img.py words <addr> <n>           dump n big-endian words
#   img.py str <addr>                 C string at addr
import bisect, pickle, struct, sys

BASE = 0x82000000
d = open('/work/hle/image.bin', 'rb').read()

def u32(a):
    o = a - BASE
    return struct.unpack('>I', d[o:o + 4])[0]

def sections():
    pe = struct.unpack('<I', d[0x3C:0x40])[0]
    nsec = struct.unpack('<H', d[pe + 6:pe + 8])[0]
    opt = struct.unpack('<H', d[pe + 20:pe + 22])[0]
    s = pe + 24 + opt
    out = []
    for i in range(nsec):
        name = d[s:s + 8].rstrip(b'\0').decode()
        vsize, va, rsize, raw = struct.unpack('<IIII', d[s + 8:s + 24])
        flags = struct.unpack('<I', d[s + 36:s + 40])[0]
        out.append((name, BASE + va, vsize, flags))
        s += 40
    return out

def funcs():
    with open('/work/hle/xref.pkl', 'rb') as f:
        fl, insns, calls, callees = pickle.load(f)
    return fl

cmd = sys.argv[1]
if cmd == 'sections':
    for n, va, sz, fl in sections():
        print(f'{n:10s} {va:08X}-{va + sz:08X} {sz:8X} flags {fl:08X}')
elif cmd == 'ptrs':
    for s in sys.argv[2:]:
        a = int(s.lower().replace('sub_', '').replace('0x', ''), 16)
        pat = struct.pack('>I', a)
        o = 0
        hits = []
        while True:
            o = d.find(pat, o)
            if o < 0:
                break
            if o % 4 == 0:
                hits.append(BASE + o)
            o += 1
        print(f'sub_{a:08X}: ' + ' '.join(f'{h:08X}' for h in hits))
elif cmd == 'tables':
    mn = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    fl = set(funcs())
    code = [x for x in sections() if x[0] == '.text']
    lo, hi = 0x820E0000, 0x828C8BA0
    runs = []
    for n, va, sz, f in sections():
        if n == '.text':
            continue
        a = va
        run = []
        while a < min(va + sz, BASE + len(d) - 4):
            v = u32(a)
            if v in fl:
                run.append((a, v))
            else:
                if len(run) >= mn:
                    runs.append(run)
                run = []
            a += 4
        if len(run) >= mn:
            runs.append(run)
    for run in runs:
        print(f'{run[0][0]:08X} {len(run):3d}: ' + ' '.join(f'{v:08X}' for a, v in run))
elif cmd == 'words':
    a = int(sys.argv[2], 16)
    n = int(sys.argv[3])
    for i in range(0, n, 8):
        print(f'{a + i * 4:08X}: ' + ' '.join(f'{u32(a + (i + j) * 4):08X}' for j in range(min(8, n - i))))
elif cmd == 'str':
    a = int(sys.argv[2], 16)
    o = a - BASE
    e = d.find(b'\0', o)
    print(d[o:e])
