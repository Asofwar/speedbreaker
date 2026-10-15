#!/usr/bin/env python3
"""Compare timedemo benchmark runs of two builds or settings (docs/timedemo.md).

    python3 scripts/bench_compare.py A_run... -- B_run... [options]
    python3 scripts/bench_compare.py ~/sb-bench/runs/A-[0-9]* -- ~/sb-bench/runs/B-[0-9]*

A run is the folder one replay wrote its report to (NFSMW_BENCH_OUT, with
summary.json and frames.csv: scripts/timedemo.sh makes <out>/<label>-<n>),
that summary.json itself, or a folder holding such folders. Side A is the
baseline, side B the change.

1. Comparability. Every run must have ended "ok", replayed the same race
   (timedemo.file_sha256) over the same window (timedemo.from and .steps)
   through the same frames (determinism.trace_hash: the game's loop, world
   step and clock of every frame in the window), drawn the same
   (determinism.draws_total within 0.1%), in the same mode and settings.
   A run that differs in any of these measured other work, so its numbers
   compare nothing: that is an error (exit 2), with --allow <key> for a
   difference that is the point of the comparison and --force for all.
   Warnings: pipelines created or draws skipped in the window (a cold
   pipeline cache), runs capped at the vblank (they measure pacing, not
   throughput), NFSMW_* environment, system or build differences.
2. A table per metric: median and min..max of each side's runs, the change
   of the medians, and a verdict: "noise" when the change is no larger than
   the wider side's spread (max - min), else better or worse. A gated
   metric (--gate) worse by more than --threshold percent and outside the
   noise is a REGRESSION.
3. Frame by frame: every run's frames.csv lined up with A's first run by
   the draws of each swap (the command processor's window starts one to
   three swaps apart from run to run, so the best shift within +-8 swaps
   wins), each side's median frame time per frame, and the 60-frame
   stretches where B is slowest, with their time in the race.

Exit: 0 no regression, 1 a regression, 2 runs not comparable or bad input.
Python 3 standard library only: it runs on a Steam Deck as it is.
"""
import argparse
import csv
import json
import math
import os
import re
import statistics
import sys

# id, label, the summary.json keys that hold it (the first one present
# wins: the bench recorder's names, debug/bench.cpp, then older spellings
# of the same number), higher is better, decimals shown.
METRICS = (
    ("timedemo_fps", "timedemo fps", ("timedemo_fps", "fps.timedemo", "window.timedemo_fps"), True, 1),
    ("fps_avg", "avg fps", ("fps.avg",), True, 1),
    ("fps_low_1pct", "1% low fps", ("fps.low_1pct",), True, 1),
    ("frame_p50", "frame p50 ms", ("frame_ms.p50",), False, 2),
    ("frame_p95", "frame p95 ms", ("frame_ms.p95",), False, 2),
    ("frame_p99", "frame p99 ms", ("frame_ms.p99",), False, 2),
    ("frame_max", "frame max ms", ("frame_ms.max",), False, 2),
    ("cp_busy", "CP busy ms", ("cpu.cp_busy_mean", "cpu.cp_busy_ms.mean", "cpu.cp_busy_ms"), False, 2),
    ("gpu_busy", "GPU busy ms", ("gpu.busy_mean", "gpu.busy_ms.mean", "gpu.busy_ms"), False, 2),
    ("wait_total", "wait ms total", ("wait_ms.total",), False, 1),
    ("wait_max", "wait ms max", ("wait_ms.max",), False, 2),
    ("hitch_late", "late swaps", ("hitches.late",), False, 0),
    ("hitch_2x", "frames >2x median", ("hitches.over_2x_median",), False, 0),
    ("hitch_50ms", "frames >=50 ms", ("hitches.over_50ms",), False, 0),
)
METRIC_IDS = tuple(m[0] for m in METRICS)

# Throughput and the slow tail: what a change is judged by. The rest of the
# table explains a change (CP or GPU, waits for loading, hitches) but is
# too noisy, or too indirect, to fail a comparison on.
DEFAULT_GATE = ("timedemo_fps", "fps_avg", "fps_low_1pct", "frame_p99")

# Environment that names a run rather than changing what it measures.
RUN_ENV = {"NFSMW_BENCH_LABEL", "NFSMW_BENCH_OUT", "NFSMW_SAVE_DIR", "NFSMW_LIFELINE_PID", "NFSMW_TIMEDEMO_FILE"}

# Mode entries that are measurements, not choices: the guest vblank follows
# the display's refresh (video/vblank_lock.h), a few hundredths of a hertz
# apart from run to run. Only a real difference (another refresh rate) is
# shown, as a warning: with the relative tolerance here.
MEASURED_MODE = {"vblank_hz": 0.01}

# Settings tables that change nothing a replay draws or computes (sound is
# muted, the pad isn't read): a difference there is shown, not refused.
HARMLESS_SETTINGS = ("audio.", "input.")

# A replay is recorded with exactly 1/60 s a frame (NFSMW_TIMEDEMO_FIXED),
# so a swap of the window is 1/60 s of race time, whatever the host's speed.
RACE_STEPS_PER_SECOND = 60.0

OPTIONS_WITH_VALUE = {"--threshold", "--gate", "--allow", "--segments", "--segment-frames", "--max-shift", "--json"}


class InputError(Exception):
    pass


# --- reading runs ------------------------------------------------------------

def natural_key(text):
    """A-2 before A-10."""
    return [int(part) if part.isdigit() else part for part in re.split(r"(\d+)", text)]


def lookup(node, key):
    """summary["a"]["b"] for "a.b", or None when any part is missing."""
    for part in key.split("."):
        if not isinstance(node, dict) or part not in node:
            return None
        node = node[part]
    return node


def number(value):
    """A finite JSON number as a float, else None (true/false are not numbers here)."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    value = float(value)
    return value if math.isfinite(value) else None


def metric_value(summary, keys):
    for key in keys:
        value = number(lookup(summary, key))
        if value is not None:
            return value
    return None


def csv_number(text):
    text = text.strip()
    try:
        return int(text)
    except ValueError:
        pass
    try:
        value = float(text)
    except ValueError:
        return None
    return value if math.isfinite(value) else None


class Run:
    def __init__(self, folder, side):
        self.folder = folder
        self.side = side
        path = os.path.join(folder, "summary.json")
        try:
            with open(path, encoding="utf-8") as f:
                self.summary = json.load(f)
        except (OSError, ValueError) as e:
            raise InputError(f"{path}: {e}")
        if not isinstance(self.summary, dict):
            raise InputError(f"{path}: not a JSON object")
        label = self.summary.get("label")
        self.name = label if isinstance(label, str) and label else os.path.basename(os.path.normpath(folder))
        self._frames = None
        self.frames_error = None

    def frames(self):
        """frames.csv as {column: [value per swap]}, or {} (frames_error says why)."""
        if self._frames is not None:
            return self._frames
        path = os.path.join(self.folder, "frames.csv")
        self._frames = {}
        try:
            with open(path, newline="", encoding="utf-8") as f:
                reader = csv.reader(f)
                header = [name.strip() for name in next(reader, [])]
                if not header:
                    self.frames_error = f"{path} is empty"
                    return self._frames
                columns = {name: [] for name in header}
                for row in reader:
                    # A short row is a report cut off mid-line (a timeout's
                    # partial report): the frames before it still count.
                    if len(row) < len(header):
                        continue
                    for name, text in zip(header, row):
                        columns[name].append(csv_number(text))
                self._frames = columns
        except OSError as e:
            self.frames_error = f"{path}: {e.strerror or e}"
        return self._frames


def expand(arg):
    """The run folders an argument names: a run folder, its summary.json, or
    a folder of run folders (in natural order: A-2 before A-10)."""
    path = os.path.expanduser(arg)
    if os.path.isfile(path):
        if os.path.basename(path) != "summary.json":
            raise InputError(f"{arg}: give a run's folder or its summary.json")
        return [os.path.dirname(os.path.abspath(path))]
    if os.path.isdir(path):
        if os.path.isfile(os.path.join(path, "summary.json")):
            return [path]
        inside = [os.path.join(path, name) for name in os.listdir(path)
                  if os.path.isfile(os.path.join(path, name, "summary.json"))]
        if inside:
            return sorted(inside, key=lambda p: natural_key(os.path.basename(p)))
        raise InputError(f"{arg}: no summary.json there, nor in a folder inside (did the run end with a report?)")
    raise InputError(f"{arg}: no such file or folder")


def load_side(args, side):
    runs, seen = [], set()
    for arg in args:
        for folder in expand(arg):
            real = os.path.realpath(folder)
            if real in seen:
                continue
            seen.add(real)
            runs.append(Run(folder, side))
    return runs


def name_runs(runs):
    """Two runs with one label (two out folders, both with an A-1) go by their folders."""
    count = {}
    for run in runs:
        count[run.name] = count.get(run.name, 0) + 1
    for run in runs:
        if count[run.name] > 1:
            run.name = os.path.normpath(run.folder)


# --- comparability -----------------------------------------------------------

def show(value):
    if value is None:
        return "missing"
    if isinstance(value, str):
        return value if len(value) <= 24 else value[:16] + ".."
    return json.dumps(value, sort_keys=True)


def describe(runs, values):
    """'1: A-1 A-2; 0: B-1' for the runs grouped by value; also the group count."""
    groups = {}
    for run, value in zip(runs, values):
        groups.setdefault(json.dumps(value, sort_keys=True), (value, []))[1].append(run.name)
    text = "; ".join(f"{show(value)}: {' '.join(names)}" for value, names in groups.values())
    return text, len(groups)


def check(runs_a, runs_b, allow):
    errors, warnings, notes = [], [], []
    every = runs_a + runs_b

    for run in every:
        status = run.summary.get("status")
        if status != "ok":
            divergence = lookup(run.summary, "determinism.divergence")
            detail = ""
            if isinstance(divergence, dict):
                detail = " (" + ", ".join(f"{k} {divergence[k]}" for k in ("kind", "loop", "step") if k in divergence) + ")"
            errors.append(f"{run.name}: status {show(status)}{detail}: the run didn't measure the whole window as replayed")

    def same(key, why, warn_missing=True, error=True, values=None):
        if key in allow:
            return
        if values is None:
            values = [lookup(run.summary, key) for run in every]
        if all(value is None for value in values):
            if warn_missing:
                warnings.append(f"no {key} in the reports: {why} not checked")
            return
        text, groups = describe(every, values)
        if groups > 1:
            (errors if error else warnings).append(f"{key} differs ({text}): {why}")

    same("timedemo.file_sha256", "not the same race file")
    same("timedemo.race_loop", "the race started on another frame")
    same("timedemo.from", "not the same window")
    same("timedemo.steps", "not the same window")
    same("determinism.trace_hash", "not the same frames replayed")
    same("window.frames", "the windows hold different numbers of game frames", warn_missing=False, error=False)

    if "determinism.draws_total" not in allow:
        draws = [number(lookup(run.summary, "determinism.draws_total")) for run in every]
        known = [d for d in draws if d is not None]
        if not known:
            warnings.append("no determinism.draws_total in the reports: draws not checked")
        else:
            middle = statistics.median(known)
            off = []
            for run, d in zip(every, draws):
                if d is None:
                    off.append(f"{run.name} missing")
                elif (middle and abs(d - middle) / middle > 0.001) or (not middle and d):
                    off.append(f"{run.name} {d:.0f} ({(d - middle) / middle * 100 if middle else 0:+.2f}%)")
            if off:
                errors.append(f"determinism.draws_total differs by more than 0.1% from the median {middle:.0f} "
                              f"({', '.join(off)}): different work drawn (pipeline skips, a setting, occlusion)")

    # Mode and settings key by key, so the message names what differs. A
    # missing mode can't be checked; settings are optional in a report.
    # (Their keys can hold dots of their own: settings' "video.vsync".)
    for section in ("mode", "settings"):
        nodes = [run.summary.get(section) if isinstance(run.summary.get(section), dict) else {} for run in every]
        keys = set()
        for node in nodes:
            keys.update(node)
        if not keys and section == "mode":
            warnings.append("no mode in the reports: settings not checked")
        for key in sorted(keys):
            name = f"{section}.{key}"
            values = [node.get(key) for node in nodes]
            if section == "mode" and key in MEASURED_MODE:
                known = [v for v in (number(v) for v in values) if v is not None]
                middle = statistics.median(known) if known else 0
                if name not in allow and middle and any(abs(v - middle) / middle > MEASURED_MODE[key] for v in known):
                    text, _ = describe(every, values)
                    warnings.append(f"{name} differs ({text}): measured on displays with other refresh rates")
                continue
            harmless = section == "settings" and key.startswith(HARMLESS_SETTINGS)
            same(name, f"not the same {section} (--allow {name} if that is the comparison)" if not harmless
                 else "a setting that doesn't change the work", error=not harmless, values=values)

    # The environment: an NFSMW_* switch may well be what A and B compare,
    # so a difference is shown, not refused.
    env_runs = [run for run in every if isinstance(run.summary.get("env"), dict)]
    names = set()
    for run in env_runs:
        names.update(name for name in run.summary["env"] if name not in RUN_ENV)
    for name in sorted(names):
        text, groups = describe(env_runs, [run.summary["env"].get(name) for run in env_runs])
        if groups > 1:
            warnings.append(f"environment {name} differs ({text})")

    for key in ("system.os", "system.machine", "system.cpu", "system.gpu", "system.display", "system.session"):
        same(key, "measured on different systems", warn_missing=False, error=False)

    for side, runs in (("A", runs_a), ("B", runs_b)):
        for key in ("build_id", "version"):
            values = [run.summary.get(key) for run in runs]
            text, groups = describe(runs, values)
            if groups > 1:
                warnings.append(f"{side}'s runs are of different builds ({key}: {text})")

    for run in every:
        created = number(lookup(run.summary, "pipelines.created")) or 0
        skips = number(lookup(run.summary, "pipelines.skips")) or 0
        if created or skips:
            warnings.append(f"{run.name}: {created:.0f} pipelines created and {skips:.0f} draws skipped in the window: "
                            "it measured shader compiles (a cold pipeline cache: replay once first, timedemo.sh --warmup)")

    capped = []
    for run in every:
        fps = number(lookup(run.summary, "fps.avg"))
        hz = (number(lookup(run.summary, "window.vblank_hz")) or number(lookup(run.summary, "mode.vblank_hz"))
              or number(lookup(run.summary, "mode.frame_rate")) or 60.0)
        flagged = lookup(run.summary, "window.capped")
        if flagged is True or (not lookup(run.summary, "mode.uncapped") and fps is not None and fps >= hz * 0.98):
            capped.append(f"{run.name} {fps:.1f} fps at {hz:g} Hz" if fps is not None else run.name)
    if capped:
        warnings.append(f"capped at the vblank ({', '.join(capped)}): such runs measure frame pacing, not how fast "
                        "frames are made; NFSMW_TIMEDEMO_UNCAPPED=1 measures throughput")

    for side, runs in (("A", runs_a), ("B", runs_b)):
        if len(runs) < 3:
            notes.append(f"{side} has {len(runs)} run{'s' if len(runs) != 1 else ''}: with fewer than 3 a side's "
                         "spread says little about the noise (5 each, in ABAB order, is better)")
    return errors, warnings, notes


# --- the table ---------------------------------------------------------------

def side_stats(values):
    if not values:
        return None
    return {"median": statistics.median(values), "min": min(values), "max": max(values), "n": len(values)}


def compare_metrics(runs_a, runs_b, gate, threshold):
    rows = []
    for metric_id, label, keys, higher, decimals in METRICS:
        a = [v for v in (metric_value(run.summary, keys) for run in runs_a) if v is not None]
        b = [v for v in (metric_value(run.summary, keys) for run in runs_b) if v is not None]
        if not a and not b:
            continue
        row = {"id": metric_id, "label": label, "higher_is_better": higher, "decimals": decimals,
               "gate": metric_id in gate, "a": side_stats(a), "b": side_stats(b),
               "delta": None, "delta_pct": None, "spread": None, "verdict": "n/a"}
        rows.append(row)
        if not a or not b:
            continue
        ma, mb = row["a"]["median"], row["b"]["median"]
        delta = mb - ma
        spread = max(max(a) - min(a), max(b) - min(b))
        pct = delta / ma * 100 if ma else None
        worse = delta < 0 if higher else delta > 0
        if delta == 0:
            verdict = "same"
        elif abs(delta) <= spread:
            # Within what a side varies by on its own: no evidence either way.
            verdict = "noise"
        elif worse:
            verdict = "REGRESSION" if row["gate"] and pct is not None and abs(pct) > threshold else "worse"
        else:
            verdict = "better"
        row.update(delta=delta, delta_pct=pct, spread=spread, verdict=verdict)
    return rows


def print_table(rows):
    header = f"{'metric':<20} {'A median':>10} {'A min..max':>18} {'B median':>10} {'B min..max':>18} {'change':>18}  verdict"
    print(header)
    print("-" * len(header))
    for row in rows:
        d = row["decimals"]

        def cells(stats):
            if not stats:
                return f"{'-':>10} {'-':>18}"
            spread = f"{stats['min']:.{d}f}..{stats['max']:.{d}f}"
            return f"{stats['median']:>10.{d}f} {spread:>18}"

        if row["delta"] is None:
            change = "-"
        else:
            change = f"{row['delta']:+.{d}f}"
            change += f" ({row['delta_pct']:+.1f}%)" if row["delta_pct"] is not None else " (from 0)"
        mark = "*" if row["gate"] else " "
        print(f"{row['label'] + mark:<20} {cells(row['a'])} {cells(row['b'])} {change:>18}  {row['verdict']}")
    print("(* gated: a regression there fails the comparison)")


# --- frame by frame ----------------------------------------------------------

def align(ref, other, max_shift):
    """The shift s with other[k + s] best matching ref[k] (draws per swap):
    the most equal swaps, then the smallest difference, then the smallest
    shift; with the share of equal swaps."""
    best = None
    need = min(len(ref), len(other)) // 2
    for shift in range(-max_shift, max_shift + 1):
        lo = max(0, -shift)
        hi = min(len(ref), len(other) - shift)
        if hi - lo <= 0 or hi - lo < need:
            continue
        equal, distance = 0, 0
        for a, b in zip(ref[lo:hi], other[lo + shift:hi + shift]):
            if a == b:
                equal += 1
            elif a is not None and b is not None:
                distance += abs(a - b)
        span = hi - lo
        key = (equal / span, -distance / span, -abs(shift))
        if best is None or key > best[0]:
            best = (key, shift)
    if best is None:
        return 0, 0.0
    return best[1], best[0][0]


def compare_frames(runs_a, runs_b, from_step, args):
    out = {"reference": runs_a[0].name, "shifts": {}, "match": {}, "excluded": {}, "frames": 0,
           "mean_delta_ms": None, "slower_share": None, "segments": []}
    warnings, notes = [], []
    ref = runs_a[0]
    ref_frames = ref.frames()
    if "draws" not in ref_frames or "frame_us" not in ref_frames:
        notes.append(f"frames: {ref.frames_error or ref.name + ': frames.csv has no draws or frame_us column'}: no frame comparison")
        return out, warnings, notes
    ref_draws = ref_frames["draws"]
    used = {"A": [], "B": []}
    for run in runs_a + runs_b:
        frames = run.frames()
        if "draws" not in frames or "frame_us" not in frames:
            out["excluded"][run.name] = run.frames_error or "no draws or frame_us column"
            continue
        if run is ref:
            shift, share = 0, 1.0
        else:
            shift, share = align(ref_draws, frames["draws"], args.max_shift)
        out["shifts"][run.name] = shift
        out["match"][run.name] = share
        if share < 0.5:
            out["excluded"][run.name] = f"its draws match {ref.name}'s on {share:.0%} of swaps at best"
            continue
        if share < 0.95:
            warnings.append(f"frames: {run.name}'s draws per swap match {ref.name}'s on {share:.1%} of swaps "
                            f"(shift {shift:+d}): the frame comparison is approximate there")
        used[run.side].append((run, shift))
    for name, why in out["excluded"].items():
        warnings.append(f"frames: {name} left out ({why})")
    if not used["A"] or not used["B"]:
        notes.append("frames: no runs on one side to compare frame by frame")
        return out, warnings, notes

    def median_at(entries, column, k):
        values = []
        for run, shift in entries:
            col = run.frames().get(column)
            j = k + shift
            if col is not None and 0 <= j < len(col) and col[j] is not None:
                values.append(col[j])
        return statistics.median(values) if values else None

    n = len(ref_draws)
    per = []  # per swap of the reference: medians of each side, or None
    for k in range(n):
        a = median_at(used["A"], "frame_us", k)
        b = median_at(used["B"], "frame_us", k)
        if a is None or b is None:
            per.append(None)
            continue
        per.append((a, b,
                    median_at(used["A"], "cp_busy_us", k), median_at(used["B"], "cp_busy_us", k),
                    median_at(used["A"], "gpu_busy_us", k), median_at(used["B"], "gpu_busy_us", k)))
    valid = [p for p in per if p is not None]
    out["frames"] = len(valid)
    if not valid:
        notes.append("frames: no swap lines up on both sides")
        return out, warnings, notes
    out["mean_delta_ms"] = sum(p[1] - p[0] for p in valid) / len(valid) / 1000
    out["slower_share"] = sum(1 for p in valid if p[1] > p[0]) / len(valid)

    width = args.segment_frames
    if n < width:
        notes.append(f"frames: fewer than {width} swaps lined up: no segments")
        return out, warnings, notes
    # Running sums of B - A and of the swaps that line up, for every window
    # of `width` swaps; a window counts only when all of it lines up.
    sums, counts = [0.0], [0]
    for p in per:
        sums.append(sums[-1] + (p[1] - p[0] if p else 0.0))
        counts.append(counts[-1] + (1 if p else 0))
    windows = [(sums[k + width] - sums[k], k) for k in range(n - width + 1) if counts[k + width] - counts[k] == width]
    windows.sort(key=lambda w: (-w[0], w[1]))
    chosen = []
    for total, start in windows:
        if len(chosen) >= args.segments or total <= 0:
            break
        if all(abs(start - other) >= width for _, other in chosen):
            chosen.append((total, start))
    swaps = ref_frames.get("swap")
    for total, start in chosen:
        span = per[start:start + width]

        def mean(index):
            values = [p[index] for p in span if p[index] is not None]
            return sum(values) / len(values) / 1000 if len(values) == len(span) else None

        a_ms, b_ms = mean(0), mean(1)
        cp_a, cp_b, gpu_a, gpu_b = mean(2), mean(3), mean(4), mean(5)
        out["segments"].append({
            "start": start,
            "race_s": [(from_step + start) / RACE_STEPS_PER_SECOND, (from_step + start + width) / RACE_STEPS_PER_SECOND],
            "swaps": [swaps[start], swaps[start + width - 1]] if swaps else [start, start + width - 1],
            "a_ms": a_ms, "b_ms": b_ms, "delta_ms": b_ms - a_ms,
            "delta_pct": (b_ms - a_ms) / a_ms * 100 if a_ms else None,
            "cp_delta_ms": cp_b - cp_a if cp_a is not None and cp_b is not None else None,
            "gpu_delta_ms": gpu_b - gpu_a if gpu_a is not None and gpu_b is not None else None,
        })
    return out, warnings, notes


def print_frames(result, width):
    shifts = ", ".join(f"{name} {shift:+d} ({result['match'][name]:.1%})" for name, shift in result["shifts"].items())
    print(f"frames: lined up with {result['reference']} by draws per swap: {shifts}")
    if result["mean_delta_ms"] is None:
        return
    print(f"  {result['frames']} swaps: B {result['mean_delta_ms']:+.3f} ms a frame on average, "
          f"slower on {result['slower_share']:.0%} of them")
    if not result["segments"]:
        print(f"  no {width}-swap stretch where B is slower")
        return
    print(f"  the {width}-swap stretches where B is slowest (race time from the window's start step, at 60 steps a second):")
    print(f"  {'#':>2} {'race s':>13} {'A swaps':>13} {'A ms':>7} {'B ms':>7} {'change':>16} {'CP':>7} {'GPU':>7}")
    for i, s in enumerate(result["segments"], 1):
        race = f"{s['race_s'][0]:.1f}-{s['race_s'][1]:.1f}"
        swaps = f"{s['swaps'][0]}-{s['swaps'][1]}"
        change = f"{s['delta_ms']:+.2f}" + (f" ({s['delta_pct']:+.1f}%)" if s["delta_pct"] is not None else "")
        cp = f"{s['cp_delta_ms']:+.2f}" if s["cp_delta_ms"] is not None else "-"
        gpu = f"{s['gpu_delta_ms']:+.2f}" if s["gpu_delta_ms"] is not None else "-"
        print(f"  {i:>2} {race:>13} {swaps:>13} {s['a_ms']:>7.2f} {s['b_ms']:>7.2f} {change:>16} {cp:>7} {gpu:>7}")
    print("  (CP and GPU: the change of busy time a frame, ms)")


# --- arguments and the run ---------------------------------------------------

def split_args(argv):
    """A's runs before the first '--', B's after it; options anywhere."""
    sides, options = ([], []), []
    side, separated = 0, False
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--" and not separated:
            separated, side = True, 1
        elif arg.startswith("-") and len(arg) > 1:
            options.append(arg)
            if arg in OPTIONS_WITH_VALUE and i + 1 < len(argv):
                i += 1
                options.append(argv[i])
        else:
            sides[side].append(arg)
        i += 1
    return sides[0], sides[1], options, separated


def parse(argv):
    parser = argparse.ArgumentParser(
        prog="bench_compare.py", usage="%(prog)s A_run... -- B_run... [options]",
        description=__doc__.split("\n\n", 2)[2], formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--threshold", type=float, default=3.0, metavar="PCT",
                        help="a gated metric worse by more than this, outside the noise, fails (default 3)")
    parser.add_argument("--gate", default=",".join(DEFAULT_GATE), metavar="IDS",
                        help=f"the metrics that can fail, comma-separated (default {','.join(DEFAULT_GATE)}; "
                             f"all: {','.join(METRIC_IDS)})")
    parser.add_argument("--allow", action="append", default=[], metavar="KEY",
                        help="a summary.json key allowed to differ, e.g. mode.vsync (repeatable)")
    parser.add_argument("--force", action="store_true", help="compare runs that aren't comparable (errors become warnings)")
    parser.add_argument("--segments", type=int, default=5, metavar="N", help="how many slowest stretches to show (5)")
    parser.add_argument("--segment-frames", type=int, default=60, metavar="N", help="a stretch's length in swaps (60)")
    parser.add_argument("--max-shift", type=int, default=8, metavar="N", help="the widest shift to line frames up with (8)")
    parser.add_argument("--no-frames", action="store_true", help="skip the frame-by-frame comparison")
    parser.add_argument("--json", metavar="PATH", help="also write the results there as JSON")
    a_args, b_args, options, separated = split_args(argv)
    args = parser.parse_args(options)
    if not separated or not a_args or not b_args:
        parser.error("give A's runs, then --, then B's runs")
    args.gate = [g.strip() for g in args.gate.split(",") if g.strip()]
    unknown = [g for g in args.gate if g not in METRIC_IDS]
    if unknown:
        parser.error(f"--gate: no metric {', '.join(unknown)} (there are {', '.join(METRIC_IDS)})")
    if args.segment_frames < 1 or args.segments < 0 or args.max_shift < 0:
        parser.error("--segments, --segment-frames and --max-shift take positive numbers")
    return args, a_args, b_args


def describe_side(side, runs):
    builds = sorted({str(run.summary.get("version") or run.summary.get("build_id") or "?") for run in runs})
    print(f"{side}: {len(runs)} run{'s' if len(runs) != 1 else ''}: {' '.join(run.name for run in runs)} ({', '.join(builds)})")


def write_json(path, result):
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2, sort_keys=True)
            f.write("\n")
    except OSError as e:
        print(f"bench_compare: can't write {path}: {e.strerror or e}", file=sys.stderr)


def main(argv):
    args, a_args, b_args = parse(argv)
    try:
        runs_a = load_side(a_args, "A")
        runs_b = load_side(b_args, "B")
    except InputError as e:
        print(f"bench_compare: {e}", file=sys.stderr)
        return 2
    shared = {os.path.realpath(r.folder) for r in runs_a} & {os.path.realpath(r.folder) for r in runs_b}
    if shared:
        print(f"bench_compare: the same run on both sides: {', '.join(sorted(shared))}", file=sys.stderr)
        return 2
    name_runs(runs_a + runs_b)

    describe_side("A", runs_a)
    describe_side("B", runs_b)
    first = runs_a[0].summary
    sha = lookup(first, "timedemo.file_sha256")
    print(f"race: {show(sha)}, window from step {show(lookup(first, 'timedemo.from'))} for "
          f"{show(lookup(first, 'timedemo.steps'))} steps, trace {show(lookup(first, 'determinism.trace_hash'))}, "
          f"{show(lookup(first, 'determinism.draws_total'))} draws")

    errors, warnings, notes = check(runs_a, runs_b, set(args.allow))
    result = {"runs": {"A": [r.name for r in runs_a], "B": [r.name for r in runs_b]},
              "comparable": not errors, "errors": errors, "warnings": warnings, "notes": notes,
              "metrics": [], "frames": None, "regressions": [], "exit_code": 2}
    if errors:
        print("NOT COMPARABLE:" if not args.force else "not comparable (--force: compared anyway):")
        for e in errors:
            print(f"  - {e}")
        if not args.force:
            print("fix the runs (or replay the race again), --allow <key> for a difference that is the comparison, "
                  "or --force to compare anyway")
            for w in warnings:
                print(f"warning: {w}")
            if args.json:
                write_json(args.json, result)
            return 2
    else:
        print("comparable: same race, window, frames, draws and mode")

    rows = compare_metrics(runs_a, runs_b, set(args.gate), args.threshold)
    result["metrics"] = rows
    frame_warnings, frame_notes = [], []
    if not args.no_frames:
        from_step = number(lookup(first, "timedemo.from")) or 0
        frames, frame_warnings, frame_notes = compare_frames(runs_a, runs_b, from_step, args)
        result["frames"] = frames
    warnings += frame_warnings
    notes += frame_notes

    for w in warnings:
        print(f"warning: {w}")
    for n in notes:
        print(f"note: {n}")
    print()
    if rows:
        print_table(rows)
    else:
        print("no metrics in the reports")
    if result["frames"] is not None and result["frames"]["shifts"]:
        print()
        print_frames(result["frames"], args.segment_frames)

    regressions = [row for row in rows if row["verdict"] == "REGRESSION"]
    result["regressions"] = [row["id"] for row in regressions]
    print()
    if regressions:
        listed = ", ".join(f"{row['label']} {row['delta_pct']:+.1f}%" for row in regressions)
        print(f"result: REGRESSION beyond {args.threshold:g}% outside the noise: {listed}")
        code = 1
    else:
        print(f"result: no regression beyond {args.threshold:g}% outside the noise")
        code = 0
    result["exit_code"] = code
    if args.json:
        write_json(args.json, result)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
