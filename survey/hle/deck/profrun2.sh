#!/bin/bash
# profrun2.sh <bundle> <label> [VAR=value...]: one uncapped 1800-step replay of
# ~/Games/SpeedBreaker-dev/<bundle> at the Deck's current TDP; inside the
# window: the APU power split (apupower.py) and every thread's CPU time
# (threadcpu.py). One summary (profsum.py) at the end. Files in ~/sb-bench:
# <label>.run.txt, .pwr.txt, .thr0.txt, .thr1.txt, runs/<label>-1/.
set -u
B="$1"; L="$2"; shift 2
D=$HOME/Games/SpeedBreaker-dev; K=$D/HLEkit
exec 9> "$HOME/sb-bench/.deck.lock"; flock -w 900 9 || exit 1
game_pid() { pgrep -f "lib/glibc/ld-linux-x86-64.so.2 .*/lib/SpeedBreaker( |$)" | head -1; }
[ -z "$(game_pid)" ] || { echo "a game is running: not measuring"; exit 1; }
cd "$K" || exit 1
rm -rf "$HOME/sb-bench/runs/$L-1"
scripts/timedemo.sh run "$L" 1 --bundle "$D/$B" --out "$HOME/sb-bench/runs" NFSMW_GAME_DIR="$D/game" NFSMW_TIMEDEMO_UNCAPPED=1 NFSMW_BENCH_FROM=1530 NFSMW_BENCH_STEPS=1800 "$@" > "$HOME/sb-bench/$L.run.txt" 2>&1 &
td=$!
LOG="$D/$B/last-run.log"
sleep 5
until grep -q "window began" "$LOG" 2>/dev/null || ! kill -0 $td 2>/dev/null; do sleep 0.5; done
PID=$(game_pid)
t0=$(date +%s.%N)
python3 "$K/threadcpu.py" "$PID" > "$HOME/sb-bench/$L.thr0.txt"
python3 "$K/survey/hle/apupower.py" 400 0.5 > "$HOME/sb-bench/$L.pwr.txt" &
pp=$!
# The threads' second snapshot 20 s into the window (the game exits right
# after it ends).
sleep 20
t1=$(date +%s.%N)
python3 "$K/threadcpu.py" "$PID" > "$HOME/sb-bench/$L.thr1.txt" 2>/dev/null
while ! grep -q "window ended" "$LOG" 2>/dev/null && kill -0 $td 2>/dev/null; do sleep 0.2; done
kill $pp 2>/dev/null; wait $td
cat /sys/class/hwmon/hwmon*/power1_cap 2>/dev/null | head -1 | sed 's/^/  power1_cap /'
grep -h -E "ok:|DIVERG|error|fail" "$HOME/sb-bench/$L.run.txt" | head -3 | cut -c1-160
python3 "$K/profsum.py" "$L" "$t0" "$t1"
