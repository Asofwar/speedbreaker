#!/usr/bin/env python3
# profcat.py <profile.txt>: NFSMW_PASS_PROFILE output by category, per frame,
# and the passes by why they began (the profile's "| after <what ended the
# pass before>, same|new targets"): their number, their GPU time and the
# GPU time in the gaps before them. "same targets" are breaks that only
# their reason forced (a resolve, a submit, an upload between draws into the
# same targets); "new targets" ones the frame's structure needs anyway.
import re, sys, collections
frames = []
cur = None
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = re.search(r'\[profile\] frame (\d+): (\d+) passes', line)
    if m:
        cur = dict(frame=int(m.group(1)), cats=collections.Counter(), n=collections.Counter(), gap=0.0,
                   why=collections.Counter(), whyN=collections.Counter(), whyGap=collections.Counter())
        frames.append(cur)
        continue
    m = re.search(r'\[profile\] total ([\d.]+) us in passes, ([\d.]+) us between', line)
    if m and cur:
        cur['total'] = float(m.group(1))
        cur['between'] = float(m.group(2))
        continue
    m = re.search(r'\[profile\]\s+([\d.]+) us\s+(\d+) draws\s+\(\+\s*([\d.]+) us before.*?\)\s+(.*)', line)
    if not m or not cur:
        continue
    us, draws, before, desc = float(m.group(1)), int(m.group(2)), float(m.group(3)), m.group(4)
    cur['gap'] += before
    mw = re.search(r'\| after (\w+)(?: \((\d+)\))?, (same|new) targets', desc)
    if mw:
        why = mw.group(1) + (f' ({mw.group(2)})' if mw.group(2) else '')
        key = f'{why}, {mw.group(3)} targets'
        cur['why'][key] += us
        cur['whyN'][key] += 1
        cur['whyGap'][key] += before
    if desc.startswith('resolve'):
        mm = re.match(r'resolve (\d+)x(\d+)', desc)
        w, h = int(mm.group(1)), int(mm.group(2))
        if w * h >= 1600 * 1600:
            k = 'resolve: shadow 1600x1600 depth'
        elif 'p640' in desc or (w, h) in ((1280, 720),):
            k = 'resolve: scene/final 1280x720 (+tiles)'
        elif w == 256:
            k = 'resolve: cube faces 256x256'
        else:
            k = 'resolve: mirror and post chain'
    elif desc.startswith('deferred resolve'):
        k = 'resolve: deferred, written back'
    elif desc.startswith('texture'):
        k = 'texture loads (untile/stage)'
    elif desc.startswith('front buffer'):
        k = 'front buffer'
    elif desc.startswith('area'):
        mm = re.match(r'area (\d+)x(\d+)', desc)
        w, h = int(mm.group(1)), int(mm.group(2))
        if draws == 0:
            k = 'pass: no draws (clear/load-store)'
        elif draws <= 1:
            k = 'pass: 1 draw (post, clears)'
        elif w * h >= 1600 * 1600:
            k = 'pass: shadow maps'
        elif w <= 256 or 'p280' in desc:
            k = 'pass: cube faces'
        elif w == 640 and h == 360:
            k = 'pass: 640x360 (mirror)'
        else:
            k = 'pass: scene'
    else:
        k = 'other: ' + desc[:30]
    cur['cats'][k] += us
    cur['n'][k] += 1
for f in frames:
    print(f"frame {f['frame']}: total {f.get('total', 0) / 1000:.2f} ms in entries, {f.get('between', 0) / 1000:.2f} ms between (sum of 'before' {f['gap'] / 1000:.2f})")
    for k in sorted(f['cats']):
        print(f"   {k:40s} {f['cats'][k] / 1000:7.2f} ms  ({f['n'][k]})")
n = len(frames) or 1
avg = collections.Counter()
for f in frames:
    for k, v in f['cats'].items():
        avg[k] += v / n
print(f'average over {len(frames)} frames:')
for k in sorted(avg):
    print(f'   {k:40s} {avg[k] / 1000:7.2f} ms')
print(f"   {'gaps (between)':40s} {sum(f.get('between', 0) for f in frames) / n / 1000:7.2f} ms")
why, whyN, whyGap = collections.Counter(), collections.Counter(), collections.Counter()
for f in frames:
    for k in f['whyN']:
        why[k] += f['why'][k] / n
        whyN[k] += f['whyN'][k] / n
        whyGap[k] += f['whyGap'][k] / n
if whyN:
    print(f'passes by why they began, a frame (average over {len(frames)} frames):')
    print(f"   {'after':40s} {'passes':>7s} {'in them':>10s} {'gaps before':>12s}")
    for k in sorted(whyN, key=lambda k: -whyN[k]):
        print(f'   {k:40s} {whyN[k]:7.1f} {why[k] / 1000:7.2f} ms {whyGap[k] / 1000:9.2f} ms')
    same = [k for k in whyN if k.endswith('same targets')]
    print(f"   {'(same targets: breaks only the reason forced)':40s} {sum(whyN[k] for k in same):7.1f} {sum(why[k] for k in same) / 1000:7.2f} ms "
          f"{sum(whyGap[k] for k in same) / 1000:9.2f} ms")
