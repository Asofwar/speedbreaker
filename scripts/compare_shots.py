#!/usr/bin/env python3
"""Compare the frame images two timedemo runs took (NFSMW_BENCH_SHOTS, docs/timedemo.md).

    python3 scripts/compare_shots.py <run A> <run B> [options]
    python3 scripts/compare_shots.py ~/sb-bench/runs/A-1 ~/sb-bench/runs/B-1 --diff /tmp/shots-diff

A run is the folder one replay wrote its report to (NFSMW_BENCH_OUT: its
shot-<step>.ppm files and summary.json), or that summary.json itself.

For each step both runs took a picture of: whether they are identical, and
if not, the largest difference of one colour value (0-255), the pixels that
differ (any colour value by more than --threshold), the PSNR, the rectangle
the differing pixels lie in, and the tiles of an 8x8 grid that hold most of
them: where in the picture, and how much. A shot passes when no more than
--tolerance percent of its pixels differ and its PSNR reaches --min-psnr.

Two runs of one build don't draw quite the same pictures, and how far apart
they are depends on the pace. On a Steam Deck (the default race, 7 shots):
- uncapped runs (NFSMW_TIMEDEMO_UNCAPPED=1, as measured; five) differed by
  small amounts over much of the picture (up to 43% of the pixels), worst
  in the sky and bright areas: a colour value by 49 at most between two
  uncapped runs (61 against a capped one), at worst 1167 pixels (0.13%) by
  more than 32, PSNR 37.4 dB at the lowest (33.6 against a capped run);
- capped runs (at the vblank's pace; two) were nearly the same: two shots
  of seven identical, the rest off by 3 at most, PSNR 55.8 dB or more.
So the defaults (--threshold 64 --tolerance 0.05 --min-psnr 30) pass
uncapped runs' noise and fail what a broken draw does: a missing, misplaced
or wrongly textured object (hundreds of pixels far off), or the whole
picture off by 8 levels or more (PSNR under 30 dB). --strict (--threshold 8
--tolerance 0.01 --min-psnr 50) is for capped runs, and catches subtle
changes too. --exact passes identical pictures only. --threshold,
--tolerance and --min-psnr, when given, override the preset's.

The runs' summary.json say which swap each picture is (the "shots" list):
pictures of different swaps are different frames, and comparing them would
mean nothing (exit 2). A shot whose run couldn't verify it (status
unverified: the game's swaps didn't keep one a frame, so the picture may be
another frame's) is compared anyway, with a warning.

--diff <dir> writes diff-<step>.ppm for each shot that differs: run A's
picture darkened, the pixels off by more than the threshold in magenta, the
others that changed in green (brighter the more they changed).

Exit: 0 every shot within the tolerance, 1 a shot beyond it, 2 the runs
can't be compared (a picture missing from one run, other sizes, other
swaps, a file that isn't a binary PPM).
Python 3 standard library only: it runs on a Steam Deck as it is.
"""
import argparse
import glob
import json
import math
import os
import re
import sys

GRID = 8  # the tiles, GRID x GRID, that say where differences are
# The gates (see the top): threshold, tolerance (% of pixels), PSNR floor (dB).
PRESETS = {
    "default": (64, 0.05, 30.0),  # uncapped runs, as measured
    "strict": (8, 0.01, 50.0),    # capped runs
    "exact": (0, 0.0, 0.0),       # identical pictures only
}
SHOT_NAME = re.compile(r"^shot-(\d+)\.ppm$")
DARKEN = bytes(v >> 2 for v in range(256))  # a diff picture's background: run A at a quarter


def read_ppm(path):
    """A binary PPM (P6, maxval 255): (width, height, the RGB bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    pos, tokens = 0, []
    while len(tokens) < 4:
        while pos < len(data) and data[pos] in b" \t\r\n":
            pos += 1
        if pos < len(data) and data[pos] == ord("#"):
            while pos < len(data) and data[pos] not in b"\r\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and data[pos] not in b" \t\r\n#":
            pos += 1
        if start == pos:
            raise ValueError("its header is cut short")
        tokens.append(data[start:pos])
    if tokens[0] != b"P6":
        raise ValueError("not a binary PPM (P6)")
    try:
        width, height, maxval = (int(t) for t in tokens[1:4])
    except ValueError:
        raise ValueError("its header's numbers aren't numbers")
    if maxval != 255 or width <= 0 or height <= 0:
        raise ValueError(f"{width}x{height}, maxval {maxval}: only 8-bit pictures are compared")
    pos += 1  # the one whitespace character after maxval
    pixels = data[pos:pos + width * height * 3]
    if len(pixels) != width * height * 3:
        raise ValueError(f"{len(pixels)} bytes of pixels, {width * height * 3} expected")
    return width, height, pixels


def write_ppm(path, width, height, pixels):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (width, height))
        f.write(pixels)


def run_folder(path):
    return os.path.dirname(os.path.abspath(path)) if os.path.isfile(path) else path


def shots_in(folder):
    """{step: path} of a run's shot-<step>.ppm files."""
    found = {}
    for path in glob.glob(os.path.join(glob.escape(folder), "shot-*.ppm")):
        m = SHOT_NAME.match(os.path.basename(path))
        if m:
            found[int(m.group(1))] = path
    return found


def summary_shots(folder):
    """{step: the summary's entry} from the run's summary.json (empty without one)."""
    try:
        with open(os.path.join(folder, "summary.json"), encoding="utf-8") as f:
            summary = json.load(f)
    except (OSError, ValueError):
        return {}
    shots = summary.get("shots") or {}
    return {int(s["step"]): s for s in shots.get("taken", []) if "step" in s}


def compare(a, b, width, height, threshold, want_diff):
    """The differences between two pictures' RGB bytes, row by row (rows
    that are equal cost one comparison)."""
    row_bytes = width * 3
    tile_w, tile_h = max(1, -(-width // GRID)), max(1, -(-height // GRID))
    tiles = [0] * (GRID * GRID)
    max_diff = changed = differing = squares = 0
    x0, y0, x1, y1 = width, height, -1, -1
    diff = bytearray(a.translate(DARKEN)) if want_diff else None
    for y in range(height):
        start = y * row_bytes
        ra, rb = a[start:start + row_bytes], b[start:start + row_bytes]
        if ra == rb:
            continue
        for x in range(width):
            i = x * 3
            d0 = abs(ra[i] - rb[i])
            d1 = abs(ra[i + 1] - rb[i + 1])
            d2 = abs(ra[i + 2] - rb[i + 2])
            if not (d0 or d1 or d2):
                continue
            changed += 1
            squares += d0 * d0 + d1 * d1 + d2 * d2
            d = max(d0, d1, d2)
            if d > max_diff:
                max_diff = d
            if d <= threshold:
                if want_diff:  # within the threshold: green, brighter the more it differs
                    diff[start + i:start + i + 3] = bytes((0, min(255, 96 + d * 16), 0))
                continue
            differing += 1
            x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
            tiles[min(y // tile_h, GRID - 1) * GRID + min(x // tile_w, GRID - 1)] += 1
            if want_diff:
                diff[start + i:start + i + 3] = b"\xff\x00\xff"
    mse = squares / float(width * height * 3)
    psnr = math.inf if mse == 0 else 10.0 * math.log10(255.0 * 255.0 / mse)
    worst = sorted(((n, t) for t, n in enumerate(tiles) if n), reverse=True)[:3]
    return {
        "identical": squares == 0,
        "max_diff": max_diff,
        "changed_pixels": changed,
        "changed_pct": 100.0 * changed / (width * height),
        "differing_pixels": differing,
        "differing_pct": 100.0 * differing / (width * height),
        "psnr": psnr,
        "bbox": [x0, y0, x1, y1] if x1 >= 0 else None,
        "tiles": [{"x": (t % GRID) * tile_w, "y": (t // GRID) * tile_h, "width": min(tile_w, width - (t % GRID) * tile_w),
                   "height": min(tile_h, height - (t // GRID) * tile_h), "pixels": n} for n, t in worst],
    }, diff


def describe(step, width, height, r, passed, threshold):
    if r["identical"]:
        return f"shot {step}: identical ({width}x{height})"
    psnr = "inf" if math.isinf(r["psnr"]) else f"{r['psnr']:.1f} dB"
    text = (f"shot {step}: {r['changed_pixels']} pixels changed ({r['changed_pct']:.2f}%), max difference {r['max_diff']}, "
            f"PSNR {psnr}; {r['differing_pixels']} off by more than {threshold} ({r['differing_pct']:.4f}%)")
    if r["bbox"]:
        x0, y0, x1, y1 = r["bbox"]
        text += f", within x {x0}..{x1} y {y0}..{y1}"
        if r["tiles"]:
            text += "; most in " + ", ".join(f"{t['x']}..{t['x'] + t['width'] - 1}x{t['y']}..{t['y'] + t['height'] - 1} ({t['pixels']})"
                                            for t in r["tiles"])
    return text + ("" if passed else "  [BEYOND TOLERANCE]")


def main(argv):
    parser = argparse.ArgumentParser(
        prog="compare_shots.py", description=__doc__.split("\n\n", 2)[2], formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("a", metavar="A", help="run A's folder (or its summary.json)")
    parser.add_argument("b", metavar="B", help="run B's folder (or its summary.json)")
    parser.add_argument("--tolerance", type=float, default=None, metavar="PCT",
                        help="the share of a shot's pixels that may differ, in percent (0.05)")
    parser.add_argument("--threshold", type=int, default=None, metavar="N",
                        help="a pixel differs when a colour value differs by more than N (64)")
    parser.add_argument("--min-psnr", type=float, default=None, metavar="DB", help="the PSNR (dB) each shot must reach (30; 0: none)")
    parser.add_argument("--strict", action="store_true", help="for capped runs: --threshold 8 --tolerance 0.01 --min-psnr 50")
    parser.add_argument("--exact", action="store_true", help="identical pictures only: --threshold 0 --tolerance 0 --min-psnr 0")
    parser.add_argument("--diff", metavar="DIR", help="write diff-<step>.ppm there for each shot that differs")
    parser.add_argument("--json", metavar="PATH", help="also write the results there as JSON")
    args = parser.parse_args(argv)
    preset = "exact" if args.exact else "strict" if args.strict else "default"
    threshold, tolerance, min_psnr = PRESETS[preset]
    args.threshold = threshold if args.threshold is None else args.threshold
    args.tolerance = tolerance if args.tolerance is None else args.tolerance
    args.min_psnr = min_psnr if args.min_psnr is None else args.min_psnr

    folders = [run_folder(args.a), run_folder(args.b)]
    problems, warnings, results = [], [], []
    found = [shots_in(f) for f in folders]
    meta = [summary_shots(f) for f in folders]
    steps = sorted(set(found[0]) | set(found[1]))
    if not steps:
        problems.append(f"no shot-<step>.ppm in {folders[0]} or {folders[1]} (NFSMW_BENCH_SHOTS)")
    for step in steps:
        missing = [side for side, f in zip("AB", found) if step not in f]
        if missing:
            problems.append(f"shot {step}: only in run {'B' if missing == ['A'] else 'A'}")
            continue
        if step in meta[0] and step in meta[1]:
            swaps = [m[step].get("swap") for m in meta]
            if swaps[0] != swaps[1]:
                problems.append(f"shot {step}: run A pictured swap {swaps[0]}, run B swap {swaps[1]}: other frames")
                continue
        for side, m in zip("AB", meta):
            status = m.get(step, {}).get("status")
            if status and status != "ok":
                warnings.append(f"shot {step}: run {side} says {status}" + (f" ({m[step]['why']})" if m[step].get("why") else ""))
        try:
            wa, ha, pa = read_ppm(found[0][step])
            wb, hb, pb = read_ppm(found[1][step])
        except (OSError, ValueError) as e:
            problems.append(f"shot {step}: {e}")
            continue
        if (wa, ha) != (wb, hb):
            problems.append(f"shot {step}: {wa}x{ha} in run A, {wb}x{hb} in run B (another internal resolution?)")
            continue
        r, diff = compare(pa, pb, wa, ha, args.threshold, bool(args.diff))
        passed = r["differing_pct"] <= args.tolerance and (args.min_psnr <= 0 or r["psnr"] >= args.min_psnr)
        r.update({"step": step, "width": wa, "height": ha, "pass": passed})
        results.append(r)
        print(describe(step, wa, ha, r, passed, args.threshold))
        if diff is not None and not r["identical"]:
            os.makedirs(args.diff, exist_ok=True)
            path = os.path.join(args.diff, f"diff-{step}.ppm")
            write_ppm(path, wa, ha, bytes(diff))
            print(f"  diff: {path}")

    for w in warnings:
        print("warning:", w)
    for p in problems:
        print("error:", p)
    failed = [r["step"] for r in results if not r["pass"]]
    identical = sum(1 for r in results if r["identical"])
    if problems:
        verdict, code = "not_comparable", 2
    elif failed:
        verdict, code = "beyond_tolerance", 1
    else:
        verdict, code = ("identical" if identical == len(results) else "within_tolerance"), 0
    gate = (f"{args.tolerance:g}% of pixels off by more than {args.threshold}"
            + (f", PSNR {args.min_psnr:g} dB" if args.min_psnr > 0 else ""))
    print(f"{len(results)} shots compared: {identical} identical, {len(results) - identical - len(failed)} within the tolerance "
          f"({gate}), {len(failed)} beyond it" + (f"; {len(problems)} not comparable" if problems else "") + f": {verdict}")
    if args.json:
        def finite(v):
            return None if isinstance(v, float) and math.isinf(v) else v
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"a": folders[0], "b": folders[1], "preset": preset, "tolerance": args.tolerance, "threshold": args.threshold,
                       "min_psnr": args.min_psnr, "verdict": verdict,
                       "shots": [{k: finite(v) for k, v in r.items()} for r in results],
                       "warnings": warnings, "problems": problems}, f, indent=2)
            f.write("\n")
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
