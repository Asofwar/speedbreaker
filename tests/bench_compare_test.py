#!/usr/bin/env python3
# scripts/bench_compare.py on made-up timedemo reports: two sides of runs
# (summary.json and frames.csv each, as debug/bench.cpp writes them) in a
# scratch folder, for each case its exit code, its --json results and the
# words its report must hold. The runs' frames.csv start one to three swaps
# apart (as the command processor's window does), so the frame comparison
# must find those shifts by the draws of each swap, and a slow stretch put
# into B's frames must come out as the worst segment, at its race time.
#   python3 tests/bench_compare_test.py
import json
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "scripts", "bench_compare.py")

failures = 0


def fail(message):
    global failures
    failures += 1
    print("  FAIL", message)


def expect(condition, message):
    if not condition:
        fail(message)


# The race as the game draws it: draws per swap, the same in every run (a
# varied sequence, so one shift lines it up and no other does). Runs see a
# window of FRAMES swaps of it from their own start offset.
FRAMES = 1200
_draws_rng = random.Random(7)
TRUE_DRAWS = [900 + _draws_rng.randrange(400) for _ in range(FRAMES + 16)]
SLOW_FROM, SLOW_TO = 600, 660  # B's slow stretch, in race swaps (10.0-11.0 s)


def mode_of(uncapped=True, vsync=False, vblank_hz=59.95):
    return {"uncapped": uncapped, "vsync": vsync, "frame_rate": "60", "aspect_ratio": "Auto",
            "internal_resolution": "100%", "anti_aliasing": False, "async_shaders": True, "vblank_hz": vblank_hz}


def settings_of(**changed):
    settings = {"video.vsync": "Off", "video.frame_rate": "60", "video.internal_resolution": "100%",
                "video.aspect_ratio": "Auto", "audio.master_volume": "100", "input.stick_deadzone": "0.1",
                "advanced.async_shaders": "On", "advanced.performance_overlay": "Off"}
    settings.update({key.replace("__", "."): value for key, value in changed.items()})
    return settings


def make_run(folder, label, fps, offset=0, slow=False, seed=1, status="ok", sha="5e" * 32,
             trace="9f3c51e0aa77d2e1", draws_total=4_500_000, mode=None, settings=None, env=None,
             pipelines=(0, 0), p99=None, frames=True):
    os.makedirs(folder)
    frame_ms = 1000.0 / fps
    # The vblank as measured: a few hundredths of a hertz apart run to run.
    summary = {
        "schema": 1, "status": status, "label": label, "version": "v0.2.0-test", "build_id": "test", "build": "test",
        "timedemo_fps": fps * 1.02,
        "timedemo": {"file": "/home/deck/sb-bench/race.jlg", "file_sha256": sha, "race_loop": 4321, "meta_race_loop": 4321,
                     "from": 0, "steps": FRAMES},
        "mode": mode or mode_of(vblank_hz=59.94 + 0.005 * (seed % 5)),
        "settings": settings or settings_of(),
        "env": env or {"NFSMW_TIMEDEMO": "replay", "NFSMW_BENCH_LABEL": label, "NFSMW_MUTE": "1",
                       "NFSMW_BENCH_OUT": "/home/deck/sb-bench/runs/" + label},
        "system": {"os": "SteamOS 3.7.13", "machine": "Valve Jupiter", "cpu": "AMD Custom APU 0405", "ram": "14.5 GB",
                   "gpu": "AMD Custom GPU 0405 (RADV VANGOGH), Mesa 25.0", "display": "1280x800 60 Hz", "session": "Game Mode"},
        "window": {"frames": FRAMES, "swaps": FRAMES, "seconds": FRAMES / fps, "main_seconds": FRAMES / fps / 1.02, "end": "steps"},
        "fps": {"avg": fps, "low_1pct": fps * 0.7, "low_01pct": fps * 0.6, "p99_fps": fps * 0.75},
        "frame_ms": {"p50": frame_ms, "p95": frame_ms * 1.2, "p99": p99 if p99 is not None else frame_ms * 1.35,
                     "p999": frame_ms * 1.8, "max": frame_ms * 2.5, "mean": frame_ms, "stdev": 1.0, "mean_abs_delta": 0.5},
        "wait_ms": {"total": 12.0, "max": 8.0},
        "cpu": {"cp_busy_mean": frame_ms * 0.5, "cp_waits": {"throttle": 1.0}},
        "gpu": {"busy_mean": frame_ms * 0.8},
        "hitches": {"late": 3, "over_2x_median": 2, "over_50ms": 0, "worst": []},
        "pipelines": {"created": pipelines[0], "skips": pipelines[1]},
        "determinism": {"divergence": None, "race_loop_matches": True, "trace_hash": trace, "draws_total": draws_total},
    }
    if status == "diverged":
        summary["determinism"]["divergence"] = {"kind": "checksum", "loop": 5123, "step": 802}
    with open(os.path.join(folder, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    if not frames:
        return
    rng = random.Random(seed)
    with open(os.path.join(folder, "frames.csv"), "w", encoding="utf-8") as f:
        f.write("swap,t_us,frame_us,draws,cp_busy_us,cp_wait_throttle_us,cp_wait_other_us,gpu_busy_us,late\n")
        t = 0
        for k in range(FRAMES):
            race = k + offset
            us = int(frame_ms * 1000 + rng.randrange(-150, 150))
            gpu = int(us * 0.8)
            if slow and SLOW_FROM <= race < SLOW_TO:
                us += 4000
                gpu += 3800
            t += us
            f.write(f"{5000 + k},{t},{us},{TRUE_DRAWS[race]},{int(us * 0.5)},100,50,{gpu},0\n")


def make_side(root, side, fps_list, offsets=None, **kw):
    folders = []
    for i, fps in enumerate(fps_list, 1):
        label = f"{side}-{i}"
        per_run = {key: (value[i - 1] if isinstance(value, list) else value) for key, value in kw.items()}
        folder = os.path.join(root, side, label)
        make_run(folder, label, fps, offset=(offsets or [0] * len(fps_list))[i - 1], seed=i * 31 + ord(side), **per_run)
        folders.append(folder)
    return folders


def compare(root, a, b, *options):
    out_json = os.path.join(root, "result.json")
    if os.path.exists(out_json):
        os.remove(out_json)
    p = subprocess.run([sys.executable, SCRIPT, *a, "--", *b, "--json", out_json, *options],
                       capture_output=True, text=True)
    result = None
    if os.path.exists(out_json):
        with open(out_json, encoding="utf-8") as f:
            result = json.load(f)
    return p.returncode, p.stdout + p.stderr, result


def verdict(result, metric):
    for row in (result or {}).get("metrics", []):
        if row["id"] == metric:
            return row["verdict"]
    return None


def case(name, build, check):
    print(name)
    before, text = failures, ""
    with tempfile.TemporaryDirectory(prefix="bench_compare_test.") as root:
        try:
            a, b, options = build(root)
            code, text, result = compare(root, a, b, *options)
            check(code, text, result)
        except Exception as e:  # a crash of this test (the script's own shows in its output)
            fail(f"{type(e).__name__}: {e}")
    if failures > before:
        print("    --- the script's output ---")
        print("    " + text.replace("\n", "\n    "))


# Within the noise: the medians differ by less than each side varies.
def noise(root):
    return (make_side(root, "A", [100.0, 101.5, 99.0], offsets=[0, 2, 1]),
            make_side(root, "B", [100.5, 99.5, 101.0], offsets=[3, 0, 1]), [])


def check_noise(code, text, result):
    expect(code == 0, f"exit {code}, wanted 0")
    expect(result and result["comparable"], "not comparable")
    expect(verdict(result, "fps_avg") == "noise", f"avg fps verdict {verdict(result, 'fps_avg')}, wanted noise")
    expect(result and result["regressions"] == [], "regressions reported")
    expect("comparable: same race" in text, "no 'comparable' line")


case("noise", noise, check_noise)


# A regression: 8% slower, tight spreads, and a slow stretch at 10-11 s.
def regression(root):
    return (make_side(root, "A", [100.0, 100.4, 99.8], offsets=[0, 2, 1]),
            make_side(root, "B", [92.0, 92.3, 91.8], offsets=[3, 0, 1], slow=True), [])


def check_regression(code, text, result):
    expect(code == 1, f"exit {code}, wanted 1")
    expect(verdict(result, "fps_avg") == "REGRESSION", f"avg fps verdict {verdict(result, 'fps_avg')}, wanted REGRESSION")
    expect("fps_avg" in (result or {}).get("regressions", []), "fps_avg not among the regressions")
    expect(verdict(result, "frame_p50") == "worse", f"frame p50 verdict {verdict(result, 'frame_p50')}, wanted worse (not gated)")
    frames = (result or {}).get("frames") or {}
    want = {"A-1": 0, "A-2": -2, "A-3": -1, "B-1": -3, "B-2": 0, "B-3": -1}
    expect(frames.get("shifts") == want, f"shifts {frames.get('shifts')}, wanted {want}")
    expect(all(share == 1.0 for share in frames.get("match", {}).values()), f"matches {frames.get('match')}")
    segments = frames.get("segments") or []
    expect(segments and abs(segments[0]["start"] - SLOW_FROM) <= 3,
           f"worst segment at {segments[0]['start'] if segments else None}, wanted ~{SLOW_FROM}")
    expect(segments and abs(segments[0]["race_s"][0] - 10.0) < 0.1, "worst segment's race time isn't ~10 s")
    expect(segments and segments[0]["delta_ms"] > 3.0, "worst segment's change is under 3 ms")
    expect(segments and segments[0]["gpu_delta_ms"] > segments[0]["cp_delta_ms"], "the slow stretch isn't GPU-side")
    expect("REGRESSION" in text and "slowest" in text, "report lacks the regression or the segments")


case("regression", regression, check_regression)


# Faster: better, exit 0.
def better(root):
    return (make_side(root, "A", [100.0, 100.4, 99.8]),
            make_side(root, "B", [108.0, 108.3, 107.9]), [])


def check_better(code, text, result):
    expect(code == 0, f"exit {code}, wanted 0")
    expect(verdict(result, "fps_avg") == "better", f"avg fps verdict {verdict(result, 'fps_avg')}, wanted better")
    expect(verdict(result, "frame_p99") == "better", f"p99 verdict {verdict(result, 'frame_p99')}, wanted better")


case("better", better, check_better)


# Not comparable: one run replayed other frames.
def other_trace(root):
    return (make_side(root, "A", [100.0, 100.4, 99.8]),
            make_side(root, "B", [100.1, 100.2, 99.9], trace=["9f3c51e0aa77d2e1", "0000000000000bad", "9f3c51e0aa77d2e1"]), [])


def check_other_trace(code, text, result):
    expect(code == 2, f"exit {code}, wanted 2")
    expect(result and not result["comparable"], "comparable")
    expect("NOT COMPARABLE" in text and "determinism.trace_hash" in text and "B-2" in text, "error doesn't name the trace and the run")


case("incomparable: trace_hash", other_trace, check_other_trace)


# Not comparable: a diverged run.
def diverged(root):
    return (make_side(root, "A", [100.0, 100.4, 99.8], status=["diverged", "ok", "ok"]),
            make_side(root, "B", [100.1, 100.2, 99.9]), [])


def check_diverged(code, text, result):
    expect(code == 2, f"exit {code}, wanted 2")
    expect("A-1: status diverged" in text and "checksum" in text and "step 802" in text, "error doesn't say how A-1 diverged")


case("incomparable: diverged", diverged, check_diverged)


# Not comparable: another race file, and draws 1% apart.
def other_race(root):
    return (make_side(root, "A", [100.0, 100.4, 99.8]),
            make_side(root, "B", [100.1, 100.2, 99.9], sha="77" * 32, draws_total=4_545_000), [])


def check_other_race(code, text, result):
    expect(code == 2, f"exit {code}, wanted 2")
    expect("timedemo.file_sha256" in text, "no race file error")
    expect("draws_total" in text, "no draws error")


case("incomparable: race file and draws", other_race, check_other_race)


# Draws within 0.1% are the same work.
def close_draws(root):
    return (make_side(root, "A", [100.0, 100.4, 99.8]),
            make_side(root, "B", [100.1, 100.2, 99.9], draws_total=4_502_000), [])


def check_close_draws(code, text, result):
    expect(code == 0, f"exit {code}, wanted 0")
    expect("draws_total" not in text, "draws within 0.1% reported")


case("draws within 0.1%", close_draws, check_close_draws)


# Another mode: refused, unless --allow names it.
OTHER_MODE = mode_of(vsync=True)


def other_mode(root):
    return (make_side(root, "A", [100.0, 100.4, 99.8]),
            make_side(root, "B", [100.1, 100.2, 99.9], mode=OTHER_MODE), [])


def check_other_mode(code, text, result):
    expect(code == 2, f"exit {code}, wanted 2")
    expect("mode.vsync differs" in text and "--allow mode.vsync" in text, "error doesn't name mode.vsync")


def allowed_mode(root):
    a, b, _ = other_mode(root)
    return a, b, ["--allow", "mode.vsync"]


def check_allowed_mode(code, text, result):
    expect(code == 0, f"exit {code}, wanted 0")
    expect(result and result["comparable"], "not comparable with --allow")


case("incomparable: mode", other_mode, check_other_mode)
case("mode difference allowed", allowed_mode, check_allowed_mode)


# Settings: a video one refuses (its key has a dot of its own), a sound or
# pad one only warns; a display at another refresh rate only warns.
def other_settings(root):
    return (make_side(root, "A", [100.0, 100.4, 99.8]),
            make_side(root, "B", [100.1, 100.2, 99.9], settings=settings_of(video__internal_resolution="150%")), [])


def check_other_settings(code, text, result):
    expect(code == 2, f"exit {code}, wanted 2")
    expect("settings.video.internal_resolution differs" in text, "error doesn't name settings.video.internal_resolution")


def harmless_settings(root):
    return (make_side(root, "A", [100.0, 100.4, 99.8]),
            make_side(root, "B", [100.1, 100.2, 99.9], mode=mode_of(vblank_hz=90.0),
                      settings=settings_of(audio__master_volume="40", input__stick_deadzone="0.2")), [])


def check_harmless_settings(code, text, result):
    expect(code == 0, f"exit {code}, wanted 0")
    warnings = " | ".join((result or {}).get("warnings", []))
    expect("settings.audio.master_volume differs" in warnings, "no warning for the volume")
    expect("settings.input.stick_deadzone differs" in warnings, "no warning for the deadzone")
    expect("mode.vblank_hz differs" in warnings and "refresh rates" in warnings, "no warning for a 90 Hz display")


def jittery_vblank(root):
    return (make_side(root, "A", [100.0, 100.4, 99.8]), make_side(root, "B", [100.1, 100.2, 99.9]), [])


def check_jittery_vblank(code, text, result):
    expect(code == 0, f"exit {code}, wanted 0")
    expect("vblank_hz" not in text, "a few hundredths of a hertz reported")


case("incomparable: settings", other_settings, check_other_settings)
case("harmless settings, another refresh rate", harmless_settings, check_harmless_settings)
case("vblank within its measuring noise", jittery_vblank, check_jittery_vblank)


# --force compares anyway: the exit code is the comparison's.
def forced(root):
    a, b, _ = other_trace(root)
    return a, b, ["--force"]


def check_forced(code, text, result):
    expect(code == 0, f"exit {code}, wanted 0")
    expect("--force: compared anyway" in text and "avg fps" in text, "no table after --force")


case("--force", forced, check_forced)


# Warnings: a cold pipeline cache, capped runs, another environment.
CAPPED_MODE = mode_of(uncapped=False, vsync=True)


def warned(root):
    return (make_side(root, "A", [59.9, 59.95, 59.9], mode=CAPPED_MODE, pipelines=[(3, 12), (0, 0), (0, 0)]),
            make_side(root, "B", [59.9, 59.9, 59.95], mode=CAPPED_MODE,
                      env={"NFSMW_TIMEDEMO": "replay", "NFSMW_MUTE": "1", "NFSMW_NEW_PATH": "1"}), [])


def check_warned(code, text, result):
    expect(code == 0, f"exit {code}, wanted 0")
    warnings = " | ".join((result or {}).get("warnings", []))
    expect("A-1: 3 pipelines created and 12 draws skipped" in warnings, "no pipelines warning")
    expect("capped at the vblank" in warnings, "no capped warning")
    expect("NFSMW_NEW_PATH" in warnings, "no environment warning")
    expect("NFSMW_BENCH_LABEL" not in warnings, "the run's own label reported as an environment difference")


case("warnings", warned, check_warned)


# A folder of runs stands for the runs in it; one run a side has no spread.
def folders(root):
    make_side(root, "A", [100.0])
    make_side(root, "B", [95.0])
    return [os.path.join(root, "A")], [os.path.join(root, "B", "B-1", "summary.json")], []


def check_folders(code, text, result):
    expect(code == 1, f"exit {code}, wanted 1 (5% off with no spread)")
    expect(result and result["runs"] == {"A": ["A-1"], "B": ["B-1"]}, f"runs {result and result['runs']}")
    expect("A has 1 run" in text, "no note about one run a side")


def threshold(root):
    a, b, _ = folders(root)
    return a, b, ["--threshold", "10"]


def check_threshold(code, text, result):
    expect(code == 0, f"exit {code}, wanted 0 (5% under a 10% threshold)")
    expect(verdict(result, "fps_avg") == "worse", f"avg fps verdict {verdict(result, 'fps_avg')}, wanted worse")


case("folders, one run a side", folders, check_folders)
case("--threshold", threshold, check_threshold)


# A run without frames.csv: the table still, the frames without it.
def no_frames(root):
    return (make_side(root, "A", [100.0, 100.4, 99.8], frames=[True, False, True]),
            make_side(root, "B", [100.1, 100.2, 99.9]), [])


def check_no_frames(code, text, result):
    expect(code == 0, f"exit {code}, wanted 0")
    frames = (result or {}).get("frames") or {}
    expect("A-2" in frames.get("excluded", {}), "A-2 without frames.csv not left out")
    expect(frames.get("frames", 0) > 0, "no frames compared")


case("a run without frames.csv", no_frames, check_no_frames)


# Bad input: no such folder, no separator.
def missing(root):
    return [os.path.join(root, "nowhere")], [os.path.join(root, "nowhere2")], []


def check_missing(code, text, result):
    expect(code == 2, f"exit {code}, wanted 2")
    expect("no such file or folder" in text, "no error for a missing folder")


case("missing input", missing, check_missing)

print("no separator")
p = subprocess.run([sys.executable, SCRIPT, "a", "b"], capture_output=True, text=True)
expect(p.returncode == 2 and "then --" in p.stderr, f"exit {p.returncode}: {p.stderr.strip()}")

print("FAILED" if failures else "ok")
sys.exit(1 if failures else 0)
