# Steam Deck measurement scripts

The scripts the numbers in `docs/hle-renderer-study.md` (sections 7 and 8)
were taken with, as they ran on a Steam Deck (`~/Games/SpeedBreaker-dev/HLEkit`
there, with the dev bundles beside it in `~/Games/SpeedBreaker-dev/<bundle>`,
the race at `~/sb-bench/race.jlg`).

- `profrun2.sh <bundle> <label> [VAR=value...]` - one uncapped 1800-step
  replay at the Deck's current TDP: the APU's power split
  (`../apupower.py`), every thread's CPU time (`threadcpu.py`) and a summary
  (`../profsum.py`). Takes the Deck's lock (`~/sb-bench/.deck.lock`).
- `series.sh <bundle> <rounds> <label:VAR=v,VAR=v>...` - `profrun2.sh` over
  the variants in turn, alternated.
- `../deck_ab.sh` - the A/B with the strict picture check (one bundle, B with
  the variables given).
- `cpstack5.sh <label> <bundle> [thread-regex] [seconds]` - perf with DWARF
  call stacks of one thread during the window.
- `gpushare2.sh`, `sharewin.sh` - other processes' GPU time (amdgpu fdinfo).
- `syscpu.py`, `threadcpu.py` - CPU time by process and thread.
- `steam-run.sh`, `hook.py` - one replay started from the Steam shortcut in
  Game Mode (the release launcher hands over while `~/sb-bench/steam-run.on`
  exists).

A series meant to outlast an SSH session runs detached:
`systemd-run --user --unit=<name> --collect bash script.sh`, with
`systemd-inhibit --what=sleep:idle ... sleep <s> &` inside it so the Deck
doesn't suspend. Check `power1_cap` (5 000 000 for 5 W) and the `[latency]`
line's `queue empty` (0 % at 5 W) before trusting a run.
