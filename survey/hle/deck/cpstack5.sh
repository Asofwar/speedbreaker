#!/bin/bash
# cpstack5.sh <label> <bundle> [thread-name-regex] [seconds]: one capped race
# replay of the bundle under the Deck lock (at the Deck's current TDP); once
# the benchmark window begins, perf samples the named thread (default
# nfsmw-cp) with DWARF call stacks. Prints the leaf libraries, the game
# functions under the libc and Vulkan driver samples, the inclusive top of
# the game's own functions and the leaf top.
set -u
LABEL="$1" BUNDLE="$2" THREAD="${3:-nfsmw-cp}" SECS="${4:-10}"
D="$HOME/Games/SpeedBreaker-dev"
OUT="$HOME/sb-bench/perf"
mkdir -p "$OUT"
game_pid() { pgrep -f "lib/glibc/ld-linux-x86-64.so.2 .*/lib/SpeedBreaker( |$)" | head -1; }
exec 9> "$HOME/sb-bench/.deck.lock"
flock -w 900 9 || { echo "the Deck stayed busy for 15 min: not measuring"; exit 1; }
[ -z "$(game_pid)" ] || { echo "a game is running: not measuring"; exit 1; }
LOG="$D/$BUNDLE/last-run.log"
"$D/tools/timedemo.sh" run "$LABEL" 1 --bundle "$D/$BUNDLE" NFSMW_GAME_DIR="$D/game" NFSMW_BENCH_FROM=1530 > "$OUT/$LABEL.timedemo.txt" 2>&1 &
TD=$!
sleep 5
until grep -q "window began" "$LOG" 2>/dev/null || ! kill -0 $TD 2>/dev/null; do sleep 0.5; done
sleep 20
PID=$(game_pid)
TID=$(for t in /proc/$PID/task/*; do n=$(cat $t/comm); [[ "$n" =~ $THREAD ]] && basename $t && break; done)
echo "pid $PID tid $TID ($THREAD)"
perf record --call-graph dwarf,16384 -F 499 -t "$TID" -o "$OUT/$LABEL.data" -- sleep "$SECS" > /dev/null 2>&1
wait $TD
flock -u 9
grep -E "ok:|DIVERG" "$OUT/$LABEL.timedemo.txt" | cut -c1-160
perf script -i "$OUT/$LABEL.data" -F ip,sym,dso 2>/dev/null > "$OUT/$LABEL.script"
python3 - "$OUT/$LABEL.script" <<'PY'
import sys, collections
samples, cur = [], []
for line in open(sys.argv[1], errors="replace"):
    line = line.rstrip("\n")
    if not line.strip():
        if cur: samples.append(cur); cur = []
        continue
    parts = line.split(None, 1)
    rest = parts[1] if len(parts) > 1 else ""
    dso = rest[rest.rfind("(") + 1:-1] if rest.endswith(")") else ""
    sym = rest[:rest.rfind("(")].strip() if "(" in rest else rest
    cur.append((sym, dso.rsplit("/", 1)[-1]))
if cur: samples.append(cur)
n = max(len(samples), 1)
print(f"{len(samples)} samples")
leafdso = collections.Counter(s[0][1] for s in samples)
print("leaf dso:", ", ".join(f"{k} {v*100/n:.1f}%" for k, v in leafdso.most_common(6)))
for lib in ("libc.so.6", "libvulkan_radeon.so"):
    callers = collections.Counter()
    for s in samples:
        if s[0][1] != lib: continue
        own = next((f for f in s if f[1] == "SpeedBreaker"), ("?", ""))
        callers[own[0][:110]] += 1
    tot = sum(callers.values())
    print(f"\n== {lib}: {tot*100/n:.1f}% of samples; first game frame:")
    for k, v in callers.most_common(12): print(f"  {v*100/n:5.1f}%  {k}")
incl = collections.Counter()
for s in samples:
    seen = set()
    for f in s:
        if f[1] == "SpeedBreaker" and f[0] not in seen:
            seen.add(f[0]); incl[f[0][:110]] += 1
print("\n== inclusive, game functions:")
for k, v in incl.most_common(30): print(f"  {v*100/n:5.1f}%  {k}")
leaf = collections.Counter(f"{s[0][0][:90]} ({s[0][1]})" for s in samples)
print("\n== leaf functions:")
for k, v in leaf.most_common(25): print(f"  {v*100/n:5.1f}%  {k}")
PY
