#!/bin/bash
# steam-run.sh: one uncapped timedemo replay of the HLEC bundle, started from
# the Steam shortcut in Game Mode (the Steam UI then sleeps, which a run
# over SSH never gets: it draws under the game there). The release launcher
# (~/Games/SpeedBreaker/speedbreaker.sh) hands over to this while
# ~/sb-bench/steam-run.on exists. Each launch replays once into
# ~/sb-bench/runs/STEAM-<n>/ (summary.json, the profile copy, the APU
# power split in pwr.txt, the TDP cap in caps.txt); the game exits by itself
# after the window. The bundle's own log: ~/Games/SpeedBreaker-dev/HLEC/last-run.log.
D=$HOME/Games/SpeedBreaker-dev
B=$HOME/sb-bench
# The Deck's lock (as the SSH series take it): a series running now ends first.
exec 9> "$B/.deck.lock"
flock -w 1800 9 || exit 1
n=1
while [ -e "$B/runs/STEAM-$n" ]; do n=$((n + 1)); done
dir=$B/runs/STEAM-$n
mkdir -p "$dir/profile"
cp -a "$B/race.jlg.save/." "$dir/profile/"
cat /sys/class/hwmon/hwmon*/power1_cap 2>/dev/null | head -1 | sed 's/^/cap_before=/' > "$dir/caps.txt"
# The power split for the whole run (the window is the last ~45 s of it).
python3 "$D/HLEkit/survey/hle/apupower.py" 600 0.5 > "$dir/pwr.txt" 2>&1 &
exec env NFSMW_FRAME_RATE=60 NFSMW_MUTE=1 NFSMW_PERF_OVERLAY=0 NFSMW_INPUT_LOG=0 NFSMW_LOG_LATENCY=0 \
    NFSMW_TIMEDEMO=replay NFSMW_TIMEDEMO_FILE="$B/race.jlg" NFSMW_SAVE_DIR="$dir/profile" \
    NFSMW_BENCH_LABEL="STEAM-$n" NFSMW_BENCH_OUT="$dir" NFSMW_GAME_DIR="$D/game" \
    NFSMW_TIMEDEMO_UNCAPPED=1 NFSMW_BENCH_FROM=1530 NFSMW_BENCH_STEPS=1800 \
    "$D/HLEC/speedbreaker.sh"
