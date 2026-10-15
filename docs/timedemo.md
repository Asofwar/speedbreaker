# Timedemo: a recorded race as a benchmark

A timedemo is one race, recorded once, that the game then plays back by
itself as often as you like: the same cars, the same traffic, the same
camera, frame for frame, on any build and at any speed of the machine. Only
the time each frame takes changes from run to run, and that is what it
measures. Use it to see whether a change to the runtime or the renderer made
the game faster or slower, by how much, and where in the race.

It needs nothing new in the game: the retail `default.xex` still holds EA's
own record/replay system, "Joylog", switched off by a flag the game never
sets. SpeedBreaker sets it (`NFSMW_TIMEDEMO`). While recording, the game
writes everything a race depends on into a log: the pad, the time each frame
was given, the random seed and when each file finished loading. While
replaying, it reads all of that back instead of the pad and the clock, so the
simulation on frame N is the same in every run. The game also checks a
checksum of its own state every frame and stops the replay at the first
frame that differs; the report then says the run diverged, instead of
quietly measuring a different race.

What it doesn't do: it doesn't replay across game versions (only the
supported USA `default.xex`), and a log recorded on one build replays on
another only as long as nothing the game itself can see has changed (see
[Limits](#limits)).

## In short

On a Steam Deck in Desktop Mode, over SSH or in Konsole, from the repo (or
any copy of `scripts/`):

```sh
# Record: drive a Quick Race, about two minutes; the game exits by itself.
scripts/timedemo.sh capture race --bundle ~/sb/A --seconds 150

# Replay A and B in turns, three times each (the first call of each side
# replays once more before, as a warmup that isn't counted).
scripts/timedemo.sh run A 1 --bundle ~/sb/A --warmup NFSMW_TIMEDEMO_UNCAPPED=1
scripts/timedemo.sh run B 1 --bundle ~/sb/B --warmup NFSMW_TIMEDEMO_UNCAPPED=1
for i in 1 2; do
    scripts/timedemo.sh run A 1 --bundle ~/sb/A NFSMW_TIMEDEMO_UNCAPPED=1
    scripts/timedemo.sh run B 1 --bundle ~/sb/B NFSMW_TIMEDEMO_UNCAPPED=1
done

# Compare.
python3 scripts/bench_compare.py ~/sb-bench/runs/A-[0-9]* -- ~/sb-bench/runs/B-[0-9]*
```

`~/sb/A` and `~/sb/B` are two unpacked SteamOS downloads (or
`scripts/steamos/make_bundle.sh` folders) of the two builds. Without
`--bundle`, the script runs the dev build, `build/main/runtime/SpeedBreaker`,
from the repo root (on a Steam Deck set up as in
[scripts/steamos/README.md](../scripts/steamos/README.md), run the script
inside the `nfsmw-build` distrobox: `distrobox enter nfsmw-build -- scripts/timedemo.sh ...`).

## 1. Record a race

```sh
scripts/timedemo.sh capture <label> [--race <path.jlg>] [--seconds <n>] [--input-script '<taps>'] [options] [VAR=value...]
```

The game starts as usual. Go to a Quick Race (a circuit of two laps is a good
benchmark: traffic, cops off, the whole track) and drive. With `--seconds
150` the recording stops 150 seconds after the race has loaded and the game
exits by itself; without it, quit the game when you are done (the recording
ends there, the menus after the race included).

Everything the game does is recorded, including the menus, so the replay
goes through the same menus too: you only drive once. To keep the menus
short (they are part of every replay's run time, though not of what is
measured), go straight to the race.

Instead of driving yourself, `--input-script` can drive (`NFSMW_INPUT_SCRIPT`,
taps by seconds since launch): `sec:BUTTON` a tap, `from-to/step:BUTTON` a tap
every `step` seconds, `from~to:BUTTON` held. Buttons: `A B X Y START BACK LB RB
LS RS UP DOWN LEFT RIGHT`, the triggers `LT RT` and the left stick `LL LR LU
LD`. A script only has to work once: whatever it did is in the log, and a
car driven badly is as good a benchmark as one driven well. For example, the
first career race from a new profile, through the taps `scripts/bench.sh`
uses to reach it, then full throttle (a starting point to adapt: check what
the screen does at each tap):

```sh
scripts/timedemo.sh capture career1 --race ~/sb-bench/career1.jlg --fresh-save --seconds 90 \
    --input-script '8-26/2:START,30-130/4:DOWN,31-131/4:A,32.5-132.5/4:START,145:A,150~260:RT'
```

What it writes (with the default `--race ~/sb-bench/race.jlg`):

| Path | What |
|---|---|
| `~/sb-bench/race.jlg` | the log, EA's format (the game writes it) |
| `~/sb-bench/race.jlg.json` | its meta: the race's first frame, where the log stops being replayable, the build, settings and `NFSMW_*` it was recorded with |
| `~/sb-bench/race.jlg.save/` | the profile the race started from (`save/` and `profile_settings.bin`), copied before the game started |
| `~/sb-bench/runs/capture/<label>/` | the game's log (`game.log`), and the copy of the profile it played on (`profile/`) |

The profile comes from the game's own data folder
(`~/.local/share/speedbreaker`, a portable bundle's `data/`), or from
`--save-from <dir>`; `--fresh-save` records with a new, empty profile. The
game records on a copy (`NFSMW_SAVE_DIR`), so the snapshot stays exactly as
the race began, and your own saves are never touched. An existing race is
never replaced without `--force`.

The recording is good if the game's log (its `[timedemo]` lines are printed
at the end) has `[timedemo] capture on: <race>` near the start and
`[timedemo] capture saved: <race> (<n> bytes; race at loop ..., log to loop
..., valid until loop ...)` at the end, with a race loop (the race did load)
and a "valid until" well after it.

## 2. Replay and measure

```sh
scripts/timedemo.sh run <label> [runs] [--race <path.jlg>] [--bundle <dir>] [--warmup] [options] [VAR=value...]
```

Each run copies `race.jlg.save/` to a fresh profile folder (`NFSMW_SAVE_DIR`: a
replay can autosave, and the profile must be the recorded one), starts the
game with `NFSMW_TIMEDEMO=replay`, and waits until it exits by itself:
loading, the menus and the race all play from the log, the recorder measures
a window of the race, writes its report and quits the game. Runs go to
`~/sb-bench/runs/<label>-<n>/` (`--out` for another folder), with the next free
`n`, so repeated calls add up (`A-1 B-1 A-2 B-2 ...`). The script prints each
run's exit code, its `[bench]` line and where its `summary.json` is.

Every run gets the same fixed environment, so that nothing outside the
change differs: `NFSMW_FRAME_RATE=60 NFSMW_MUTE=1 NFSMW_PERF_OVERLAY=0
NFSMW_INPUT_LOG=0 NFSMW_LOG_LATENCY=0`. More goes on the command line as
`VAR=value` (after the fixed ones, so they win). `NFSMW_*` variables already set
in your shell apply too; the script names them once.

For good numbers:

- **`NFSMW_TIMEDEMO_UNCAPPED=1`** for throughput: frames are released
  without waiting for the vblank, so the run shows how fast frames can be
  made. Without it, the game is held to 60 fps and a run on a fast machine
  only measures the pacing (bench_compare warns about such "capped" runs).
  It applies inside the recorder's window only: before it, the replay is
  paced as the capture was, because movies and the race's intro run on real
  time, and a replay that rushes through them can fall out of step with the
  log where the intro hands over to the race.
- **`NFSMW_BENCH_FROM`** past the race's intro, so the window measures
  driving: for the race captured with the default input script, the intro
  ends about 1520 world steps after the race loads (`NFSMW_BENCH_FROM=1530`).
- **`--warmup`** once per build before measuring: the first run after a new
  build compiles shaders (the pipeline cache) and loads files cold. Its report
  goes to `~/sb-bench/runs/warmup/<label>/` and isn't compared.
- **A and B in turns** (ABAB...), at least three each: a Steam Deck warms up
  and clocks down over consecutive runs, which would favour whichever side ran
  first.
- The same settings for both sides: the bundles share the game's settings
  file, unless one is portable.

### Game Mode, through Steam

Over SSH, the script shows the game on the Deck's screen: in Desktop Mode
through the desktop session's display (it borrows `DISPLAY` and the rest from
plasmashell), in Game Mode through gamescope's (`DISPLAY=:0`). Game Mode,
though, shows the window of the game Steam started, so a run started from
SSH may play behind Steam's own screen. To measure in Game Mode, start the
replay from Steam instead, with the bundle's shortcut and these launch options (one
run per launch; a fresh copy of the profile each time):

```
NFSMW_TIMEDEMO=replay NFSMW_TIMEDEMO_FILE=/home/deck/sb-bench/race.jlg NFSMW_SAVE_DIR=/home/deck/sb-bench/save-run NFSMW_TIMEDEMO_UNCAPPED=1 NFSMW_BENCH_LABEL=A NFSMW_FRAME_RATE=60 NFSMW_MUTE=1 NFSMW_PERF_OVERLAY=0 NFSMW_INPUT_LOG=0 NFSMW_LOG_LATENCY=0 %command%
```

and before each launch: `rm -rf ~/sb-bench/save-run && cp -a ~/sb-bench/race.jlg.save ~/sb-bench/save-run`.
Reports then go to `~/.local/share/speedbreaker/logs/bench/` (`NFSMW_BENCH_OUT`
for a folder of your own).

## 3. Compare

```sh
python3 scripts/bench_compare.py <A runs...> -- <B runs...> [--threshold 3] [--allow <key>] [--force] [--json <path>]
```

A run is a run's folder, its `summary.json`, or a folder of run folders. A is
the baseline, B the change. Python 3's standard library is all it needs (the
Steam Deck's `python3` does).

1. **Comparability.** Every run must have ended `ok`, replayed the same race
   file (`timedemo.file_sha256`) over the same window (`timedemo.from`,
   `timedemo.steps`) through the same frames (`determinism.trace_hash`), drawn
   the same (`determinism.draws_total` within 0.1%) in the same mode and
   settings. Otherwise the runs measured different work, and the comparison
   stops with an error naming the key and which runs have which value (exit
   2). `--allow mode.vsync` (any key) accepts a difference that is the point
   of the comparison; `--force` compares anyway. Warnings, not errors:
   pipelines created or draws skipped in the window (a cold pipeline cache:
   use `--warmup`), runs capped at the vblank, differing `NFSMW_*`
   environment (often exactly what A and B compare), sound and pad settings,
   systems, builds within one side, a display at another refresh rate.
2. **The table.** For each metric (timedemo fps, avg fps, 1% low, frame time
   p50/p95/p99/max, CP and GPU busy, waits for loading, hitches), the median
   and min..max of each side's runs and the change of the medians. The
   verdict is `noise` when the change is no larger than the wider side's
   spread (max - min), otherwise `better` or `worse`; a gated metric (`*`:
   timedemo fps, avg fps, 1% low, p99 by default, `--gate` to choose) worse by
   more than `--threshold` percent (3) and outside the noise is a
   `REGRESSION`. With one run a side there is no spread, and every difference
   counts.
3. **Frame by frame.** Every run's `frames.csv` is lined up with A's first run
   by the number of draws of each swap (the measured window starts a swap or
   three apart from run to run; the best shift within 8 swaps wins), each
   side's median frame time is taken per frame, and the 60-frame stretches
   where B is slowest are printed with their time in the race (from the
   window's start, at 60 frames a second of race time) and how much of the
   change is the CP's and the GPU's busy time. That is where to look in the
   race for what got slower.

## Switches

All off by default: without them the game behaves exactly as without this
feature.

| Variable | Meaning |
|---|---|
| `NFSMW_TIMEDEMO=capture\|replay` | record, or replay as a benchmark |
| `NFSMW_TIMEDEMO_FILE=<path.jlg>` | the log (default `<data folder>/timedemo/race.jlg`); beside it its meta, `<path.jlg>.json`, and the profile snapshot, `<path.jlg>.save/` |
| `NFSMW_TIMEDEMO_FIXED=1\|0` | capture: give every frame exactly 1/60 s (default 1) |
| `NFSMW_TIMEDEMO_CAPTURE_SECONDS=<s>` | capture: stop and exit this long after the race has loaded (0: at the quit) |
| `NFSMW_TIMEDEMO_UNCAPPED=1` | replay: release frames without waiting for the vblank |
| `NFSMW_BENCH_FROM=<steps>` | the window starts this many world steps (1/60 s each) after the race has loaded (0) |
| `NFSMW_BENCH_STEPS=<n>` | the window's length in world steps (default: to 60 steps before the log's replayable end) |
| `NFSMW_BENCH_LABEL=<text>` | the run's name in its report |
| `NFSMW_BENCH_OUT=<dir>` | where the report goes (default `<data folder>/logs/bench/<date>_<time>_<label>/`) |
| `NFSMW_BENCH_TIMEOUT=<s>` | a partial report and exit after this long (900) |
| `NFSMW_BENCH_EXIT=0` | stay in the game after the report (the default is to exit) |
| `NFSMW_SAVE_DIR=<dir>` | an absolute path: `save/` and `profile_settings.bin` there instead of the data folder's |

## The report

In the run's folder (`NFSMW_BENCH_OUT`):

- `summary.json`: `status`, `label`, `version`, `build_id`; `timedemo_fps`
  (the classic figure: the window's game frames over their time);
  `timedemo{file_sha256, race_loop, from, steps}`; `window{frames, swaps,
  seconds, ...}`; `fps{avg, low_1pct, low_01pct, p99_fps}`; `frame_ms` and
  `game_frame_ms{p50, p95, p99, p999, max, mean, stdev, mean_abs_delta}`;
  `wait_ms{total, max}` (the game thread waiting for file loads, apart from
  render time); `cpu{cp_busy_mean, cp_waits{...}}`; `gpu{busy_mean,
  exec_mean}` (exec with `NFSMW_GPU_TIMING=1`); `hitches{late,
  over_2x_median, over_50ms, worst[]}`; `pipelines{created, skips}`;
  `memory`; `determinism{divergence, trace_hash, draws_total}`; `mode`,
  `settings`, `env`, `system`.
- `frames.csv`: one row per swap in the window: `swap, t_us, frame_us,
  draws, cp_busy_us, cp_wait_throttle_us, cp_wait_other_us, gpu_busy_us,
  late`.
- `main.csv`: one row per game frame: `loop, step, t_us, frame_us, wait_us,
  flow`.
- A `[bench]` line in the game's log: `[bench] A-1 ok: 7200 frames 98.4 s |
  timedemo 73.2 fps, 1% low 51.0 | p50 13.1 p99 21.4 max 48.2 ms | ...`.

## Exit codes

| | Code | Meaning |
|---|---|---|
| the game, replaying | 0 | ok: the window measured, the report written |
| | 2 | setup failed (the log didn't open, or is from another `default.xex`), the log ended before the window did, or the bench timed out |
| | 3 | diverged: the game's checksum found the replay out of step (the log has `[timedemo] DIVERGENCE kind=... loop=... step=...`) |
| | 4 | interrupted: the runtime's own menu or a notice came up during the replay |
| `timedemo.sh` | 0 | every run ended ok with a report |
| | 1 | a run didn't (each run's code is printed; 124: stopped at `--timeout`) |
| | 2 | bad arguments, or a missing race or profile snapshot |
| `bench_compare.py` | 0 | comparable, and no regression beyond the threshold outside the noise |
| | 1 | a regression |
| | 2 | the runs aren't comparable, or bad input |

## Limits

- **One `default.xex`.** The guest addresses the feature uses are those of
  the supported USA release only.
- **The same `ppc/` build.** A replay stays in step while the recompiled
  code computes exactly the same floating point: the same recompiler output,
  the same compiler and flags. A rebuilt `ppc/` (another compiler, other
  flags) or a runtime change the game can see (memory it is given, file
  sizes, timing it is told) can make an old log diverge; the run then ends
  with code 3 at the first frame that differs, and the race has to be
  recorded again on the new build. A change to the renderer alone leaves the
  game's own computation as it was, which is what A/B runs of one race are
  for.
- **The same settings.** What the game itself sees must match the recording:
  the aspect ratio (ultrawide), Frame Rate 60, the language. `race.jlg.json`
  lists what it was recorded with; the checksum has the last word.
  Rendering settings may differ from the recording, but must match between A
  and B (bench_compare checks).
- **The same profile.** Every replay must start from the recorded profile:
  the snapshot in `race.jlg.save/`, a fresh copy per run (the script does this).
  The gearbox, the camera and the units come from the profile, and a replay
  may autosave.
- **Only what is in the log.** The runtime's own menu or notices during a
  replay aren't in it: the run ends as interrupted (code 4). During a
  recording, the log stays replayable only up to the first such notice (the
  meta's end). Don't open the runtime's menu while recording or replaying.
- **Uncapped runs** play the race faster than real time while sound,
  movies and loading screens keep real time; file loads then can't keep up,
  and the game waits for them inside its frames. That time is `wait_ms`, kept
  apart from render time: if it grows, the run is limited by streaming, not
  by rendering.
- **A crash while recording** loses up to the last 10 seconds of the log.
- Timedemo runs keep a 1 MB log buffer in the game's memory: compare them
  with each other, not with normal play.
