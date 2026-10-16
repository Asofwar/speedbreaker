#!/usr/bin/env python3
# Motion vector prototype (NFSMW_MV): compare timedemo shots of several runs
# with a reference run, by region of the race's chase-camera frame.
#
#   python3 scripts/mv_eval.py --ref <run dir> NAME=<run dir> ... [--out <dir>] [--steps 4600,4800]
#
# A run dir holds shot-<step>.ppm (NFSMW_BENCH_SHOTS, docs/timedemo.md). A
# reference taken at a higher internal resolution (NFSMW_RENDER_SCALE=2:
# 2560x1440 shots) is box-filtered down to 1280x720: a supersampled picture
# of the same frame, the ground truth for anti-aliasing and ghosting. For
# each step and run it prints the PSNR against the reference over the frame
# without the HUD, and over the player's car, the road and the rest (the
# regions are the race camera's, see REGIONS), and with --out writes a
# picture per step: the runs' crops of the car and of the road, side by side,
# and each run's error (x4) beside it.
#
# Needs numpy and Pillow (not the Steam Deck's python3: run it on a PC).
import argparse
import glob
import os
import re
import sys

import numpy as np
from PIL import Image

# 1280x720 frame, chase camera (the default race, from step ~4500 on).
HUD = [  # x0, y0, x1, y1: the HUD (identical in every run) is left out
    (488, 58, 792, 180),    # rear-view mirror
    (968, 60, 1222, 202),   # position, lap time, progress
    (50, 440, 276, 650),    # minimap, nitrous
    (996, 440, 1226, 672),  # speedometer
]
REGIONS = {
    'car': (440, 380, 840, 700),   # the player's car
    'road': (0, 380, 1280, 720),   # the road in front (less the car and HUD)
}


def load(path):
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.float64)
    h, w = a.shape[:2]
    if (w, h) != (1280, 720):
        fx, fy = w // 1280, h // 720
        if fx * 1280 != w or fy * 720 != h or fx != fy:
            sys.exit(f'{path}: {w}x{h} is not a whole multiple of 1280x720')
        a = a.reshape(720, fy, 1280, fx, 3).mean(axis=(1, 3))
    return a


def masks():
    m = {}
    hud = np.zeros((720, 1280), bool)
    for x0, y0, x1, y1 in HUD:
        hud[y0:y1, x0:x1] = True
    frame = ~hud
    car = np.zeros_like(hud)
    x0, y0, x1, y1 = REGIONS['car']
    car[y0:y1, x0:x1] = True
    road = np.zeros_like(hud)
    x0, y0, x1, y1 = REGIONS['road']
    road[y0:y1, x0:x1] = True
    m['frame'] = frame
    m['car'] = car & frame
    m['road'] = road & ~car & frame
    m['rest'] = frame & ~road
    return m


def psnr(a, b, mask):
    d = (a - b)[mask]
    mse = float(np.mean(d * d)) if d.size else 0.0
    return 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref', required=True)
    ap.add_argument('--out')
    ap.add_argument('--steps')
    ap.add_argument('runs', nargs='+', help='NAME=<run dir>')
    args = ap.parse_args()
    runs = []
    for r in args.runs:
        name, _, d = r.partition('=')
        runs.append((name, d))
    steps = sorted(int(re.search(r'shot-(\d+)', f)[1]) for f in glob.glob(os.path.join(args.ref, 'shot-*.ppm')))
    if args.steps:
        steps = [s for s in steps if s in {int(x) for x in args.steps.split(',')}]
    m = masks()
    cols = ['frame', 'car', 'road', 'rest']
    print('PSNR (dB) against the reference, HUD left out')
    print('%-6s %-14s ' % ('step', 'run') + ' '.join('%7s' % c for c in cols))
    totals = {name: {c: [] for c in cols} for name, _ in runs}
    for s in steps:
        ref = load(os.path.join(args.ref, f'shot-{s}.ppm'))
        shots = []
        for name, d in runs:
            p = os.path.join(d, f'shot-{s}.ppm')
            if not os.path.exists(p):
                continue
            a = load(p)
            shots.append((name, a))
            vals = [psnr(a, ref, m[c]) for c in cols]
            for c, v in zip(cols, vals):
                totals[name][c].append(v)
            print('%-6d %-14s ' % (s, name) + ' '.join('%7.2f' % v for v in vals))
        if args.out and shots:
            os.makedirs(args.out, exist_ok=True)
            crops = [('car', (460, 400, 820, 700)), ('road', (0, 380, 440, 720)), ('far', (380, 180, 900, 400))]
            rows = [('reference', ref)] + shots
            tiles = []
            for name, a in rows:
                row = []
                for _, (x0, y0, x1, y1) in crops:
                    row.append(a[y0:y1, x0:x1])
                    err = np.clip(np.abs(a - ref)[y0:y1, x0:x1] * 4, 0, 255)
                    row.append(err)
                h = max(t.shape[0] for t in row)
                row = [np.pad(t, ((0, h - t.shape[0]), (0, 4), (0, 0))) for t in row]
                tiles.append(np.concatenate(row, axis=1))
            w = max(t.shape[1] for t in tiles)
            tiles = [np.pad(t, ((0, 4), (0, w - t.shape[1]), (0, 0))) for t in tiles]
            Image.fromarray(np.concatenate(tiles, axis=0).astype(np.uint8)).save(os.path.join(args.out, f'crops-{s}.png'))
            with open(os.path.join(args.out, f'crops-{s}.txt'), 'w') as f:
                f.write('rows: ' + ', '.join(n for n, _ in rows) + '\ncolumns: car, its error x4, road, error, far, error\n')
    print('\nmean over steps')
    for name, _ in runs:
        print('%-21s ' % name + ' '.join('%7.2f' % (np.mean(totals[name][c]) if totals[name][c] else float('nan')) for c in cols))


if __name__ == '__main__':
    main()
