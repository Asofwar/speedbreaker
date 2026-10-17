#!/usr/bin/env python3
# dirtyscan.py: functions outside the D3D library that update a D3D dirty
# mask inline: "ld rX,D(rB) ... or/oris/ori rX,... ; std rX,D(rB)" with the
# device's masks at +16..+48, either directly (D in 16..48) or through
# rB = device + 16 (D in 0..32, with "addi rB,rDev,16" earlier).
import pickle, re, sys, collections
funcs, insns, calls, callees = pickle.load(open('/work/hle/xref.pkl', 'rb'))
D3D_LO, D3D_HI = 0x82587000, 0x825B0000
rx_ld = re.compile(r'^ld (r\d+),(-?\d+)\((r\d+)\)$')
rx_std = re.compile(r'^std (r\d+),(-?\d+)\((r\d+)\)$')
rx_addi16 = re.compile(r'^addi (r\d+),(r\d+),16$')
hits = collections.OrderedDict()
for fn in funcs:
    if D3D_LO <= fn < D3D_HI:
        continue
    plus16 = set()
    lds = {}
    found = []
    for a, t in insns[fn]:
        t = t.strip()
        m = rx_addi16.match(t)
        if m:
            plus16.add(m.group(1))
            continue
        m = rx_ld.match(t)
        if m:
            lds[(m.group(3), int(m.group(2)))] = a
            continue
        m = rx_std.match(t)
        if m:
            key = (m.group(3), int(m.group(2)))
            d = key[1]
            if key in lds:
                if (key[0] in plus16 and d in (0, 8, 16, 24, 32)) or (key[0] not in plus16 and d in (16, 24, 32, 40, 48)):
                    found.append((a, key[0], d + (16 if key[0] in plus16 else 0)))
    if found:
        hits[fn] = found
which = collections.Counter()
for fn, fl in hits.items():
    for a, r, d in fl:
        which[d] += 1
print(f'{len(hits)} functions outside D3D update a mask-like qword at device+16..48 (sites by offset: {dict(sorted(which.items()))})')
for fn, fl in hits.items():
    print(f'  sub_{fn:08X} ({len(calls.get(fn, []))} callers): ' + ' '.join(f'{a:08X}:+{d}' for a, r, d in fl))
