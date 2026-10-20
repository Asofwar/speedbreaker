#!/usr/bin/env python3
# tracesum.py <file.prof>: the D3D frame trace (d3d_profile D3DPROF_TRACE) as
# passes: render targets, draws, resolves (decoded destination texture).
import re, sys

FMT = {6: 'k_8_8_8_8', 7: 'k_2_10_10_10', 22: 'k_24_8', 23: 'k_24_8_FLOAT', 26: 'k_16_16', 32: 'k_16_16_16_16_FLOAT',
       31: 'k_16_16_FLOAT', 36: 'k_32_FLOAT', 37: 'k_32_32_FLOAT', 25: 'k_16_16_16_16', 2: 'k_8', 4: 'k_5_6_5', 38: 'k_32_32_32_32_FLOAT',
       10: 'k_8_8', 18: 'k_DXT1', 19: 'k_DXT2_3', 20: 'k_DXT4_5', 30: 'k_16_FLOAT', 24: 'k_16', 54: 'k_2_10_10_10_AS_16_16_16_16'}
BPP = {6: 4, 7: 4, 22: 4, 23: 4, 26: 4, 32: 8, 31: 4, 36: 4, 37: 8, 25: 8, 2: 1, 4: 2, 38: 16, 10: 2, 30: 2, 24: 2}

def fetch(words):
    w = [int(x, 16) for x in words]
    if len(w) < 10:
        return None
    d0, d1, d2, d5 = w[4], w[5], w[6], w[9]
    fmt = d1 & 0x3F
    tiled = d0 >> 31
    pitch = ((d0 >> 22) & 0x1FF) * 32
    base = d1 & 0xFFFFF000
    width = (d2 & 0x1FFF) + 1
    height = ((d2 >> 13) & 0x1FFF) + 1
    depth = (d2 >> 26) + 1
    dim = (d5 >> 9) & 3
    return dict(fmt=fmt, fmtname=FMT.get(fmt, str(fmt)), tiled=tiled, pitch=pitch, base=base, w=width, h=height, d=depth,
                dim=['1D', '2D', '3D', 'cube'][dim])

def surf(words):
    w = [int(x, 16) for x in words]
    if len(w) < 8:
        return None
    size = w[6]
    return dict(w=(size & 0x1FFF) + 1, h=((size >> 13) & 0x1FFF) + 1, info=w[5], edram=w[7])

lines = open(sys.argv[1]).read().split('\n')
start = [i for i, l in enumerate(lines) if l.startswith('# trace from')]
if not start:
    sys.exit('no trace')
lines = lines[start[0]:]
rt = [None] * 4
ds = None
vp = None
total_draws = 0
resolves = []
passes = []
cur = None
for l in lines:
    m = re.match(r'T \d+ SetRenderTarget index (\d) surface ([0-9A-F]+) \[(.*?)\]', l)
    if m:
        i = int(m.group(1))
        rt[i] = None if m.group(2) == '00000000' else (m.group(2), surf(m.group(3).split()))
        continue
    m = re.match(r'T \d+ SetDepthStencilSurface surface ([0-9A-F]+) \[(.*?)\]', l)
    if m:
        ds = None if m.group(1) == '00000000' else (m.group(1), surf(m.group(2).split()))
        continue
    m = re.match(r'T \d+ SetViewport (.*)', l)
    if m:
        vp = m.group(1)
        continue
    m = re.match(r'T \d+\s+draws: indexed (\d+), non-indexed (\d+), UP (\d+), BeginVertices (\d+)', l)
    if m:
        n = sum(int(x) for x in m.groups())
        total_draws += n
        key = (tuple(r[0] if r else None for r in rt), ds[0] if ds else None)
        if cur is None or cur['key'] != key:
            cur = dict(key=key, rt=[r for r in rt], ds=ds, draws=0, vp=vp, resolves=[])
            passes.append(cur)
        cur['draws'] += n
        continue
    m = re.match(r'T \d+ (Resolve|EndTiling) flags ([0-9A-F]+) rect \[(.*?)\] dest ([0-9A-F]+) \[(.*?)\]', l)
    if m:
        f = fetch(m.group(5).split())
        src = ds if (int(m.group(2), 16) & 4) and m.group(1) == 'Resolve' else rt[0]
        r = dict(kind=m.group(1), flags=int(m.group(2), 16), rect=m.group(3), dest=m.group(4), f=f, src=src, line=l)
        resolves.append(r)
        if cur is not None:
            cur['resolves'].append(r)
        continue
    if l.startswith('T ') and ('BeginTiling' in l or 'ClearF' in l or 'Swap' in l or 'SetPredication' in l):
        print('   ' + l[:230])
print(f'draws traced: {total_draws}, passes (render target set changes with draws): {len(passes)}, resolves: {len(resolves)}')
for p in passes:
    c = p['rt'][0]
    d = p['ds']
    cs = f'{c[1]["w"]}x{c[1]["h"]}' if c and c[1] else '-'
    dss = f'{d[1]["w"]}x{d[1]["h"]}' if d and d[1] else '-'
    extra = sum(1 for r in p['rt'][1:] if r)
    print(f'pass color {cs}{" +" + str(extra) if extra else ""} depth {dss} vp {p["vp"]}: {p["draws"]} draws')
    for r in p['resolves']:
        f = r['f']
        if f:
            bpp = BPP.get(f['fmt'], 4)
            rect = r['rect'] if r['rect'] not in ('null',) else 'whole'
            print(f'    {r["kind"]} flags {r["flags"]:X} rect {rect} -> {f["dim"]} {f["w"]}x{f["h"]}{"x%d" % f["d"] if f["d"] > 1 else ""} {f["fmtname"]} '
                  f'{"tiled" if f["tiled"] else "linear"} @{f["base"]:08X} ({f["w"] * f["h"] * bpp / 1e6:.2f} MB a level/face)')
