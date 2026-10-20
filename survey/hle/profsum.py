#!/usr/bin/env python3
# profsum.py <file.prof> <from> <to> [names.txt]: per-frame averages between two
# dumps of d3d_profile / cp_profile.
import sys, re, collections
path, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
names = {}
if len(sys.argv) > 4:
    for line in open(sys.argv[4]):
        line = line.strip()
        if line and not line.startswith('#'):
            k, v = line.split(None, 1)
            names[int(k.replace('sub_', ''), 16)] = v
d3d = {}
cp = {}
cur = None
for line in open(path):
    line = line.rstrip('\n')
    m = re.match(r'# d3dprof swap (\d+)', line)
    if m:
        cur = ('d3d', int(m.group(1)))
        d3d[cur[1]] = {}
        continue
    m = re.match(r'# cpprof frame (\d+)', line)
    if m:
        cur = ('cp', int(m.group(1)))
        cp[cur[1]] = {}
        continue
    if line.startswith('# end'):
        cur = None
        continue
    if cur is None or line.startswith('fn,'):
        continue
    if cur[0] == 'd3d':
        f = line.split(',')
        d3d[cur[1]][int(f[0], 16)] = [int(x) for x in f[1:]]
    else:
        parts = line.split()
        if parts[0] == 't3':
            op = int(parts[1], 16)
            cp[cur[1]][f't3_{op:02X}'] = (int(parts[3]), int(parts[5]), int(parts[7]))
        elif parts[0] in ('t0ByRange', 'setConst'):
            cp[cur[1]][parts[0]] = [int(x) for x in parts[1:]]
        else:
            for i in range(0, len(parts), 2):
                cp[cur[1]][parts[i]] = int(parts[i + 1])
n = b - a
print(f'== D3D library calls per frame (guest swaps {a}..{b}, {n} frames)')
A, B = d3d[a], d3d[b]
rows = []
for fn in B:
    va = A.get(fn, [0] * 6)
    vb = B[fn]
    dl = [(y - x) / n for x, y in zip(va, vb)]
    if dl[0] > 0:
        rows.append((fn, dl))
rows.sort(key=lambda r: -r[1][1])
tot_ext = sum(r[1][1] for r in rows)
tot_ring = sum(r[1][2] for r in rows)
tot_after = sum(r[1][4] for r in rows)
tot_ns = sum(r[1][3] for r in rows)
print(f'top-level calls/frame {tot_ext:.0f}, ring bytes/frame {tot_ring:.0f} (+{tot_after:.0f} after, by game code), top-level time {tot_ns / 1e6:.2f} ms/frame')
print(f'{"fn":12s} {"calls":>9s} {"toplvl":>8s} {"ringB":>9s} {"B/call":>7s} {"us":>8s} {"afterB":>8s}  name')
for fn, dl in rows:
    if dl[1] < 0.5 and dl[0] < 50:
        continue
    bpc = dl[2] / dl[1] if dl[1] else 0
    print(f'sub_{fn:08X} {dl[0]:9.1f} {dl[1]:8.1f} {dl[2]:9.0f} {bpc:7.0f} {dl[3] / 1e3:8.1f} {dl[4]:8.0f}  {names.get(fn, "")}')
ca, cb = cp.get(a), cp.get(b)
if ca and cb:
    print(f'\n== Command processor per frame (frames {a}..{b})')
    for k in ('dwords', 'ibDwords', 'ibs', 't0Packets', 't0Regs', 't1Packets', 't2Dwords', 'draws', 'drawsSkipped', 'loadAluDwords'):
        print(f'{k:14s} {(cb[k] - ca[k]) / n:12.1f}')
    for k, lab in (('t0ByRange', ['2000-23FF', '4000-47FF', '4800-48BF', '4900-4927', '<2000', 'other']),
                   ('setConst', ['ALU', 'fetch', 'bool', 'loop', 'regs', '5', '6', '7'])):
        print(k + ': ' + ', '.join(f'{l} {(y - x) / n:.0f}' for l, x, y in zip(lab, ca[k], cb[k])))
    ops = sorted(k for k in cb if k.startswith('t3_'))
    for k in ops:
        x = ca.get(k, (0, 0, 0))
        y = cb[k]
        print(f'{k}: packets {(y[0] - x[0]) / n:9.1f} dwords {(y[1] - x[1]) / n:10.1f} skipped {(y[2] - x[2]) / n:8.1f}')
