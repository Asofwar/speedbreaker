#!/bin/bash
# deck_ab.sh <bundle> <pairs> <label> [VAR=value...]: phase 1's go/no-go
# measurement on a Steam Deck at a 5 W TDP limit (docs/hle-renderer-study.md,
# "Фаза 0–1"). One bundle, two settings: A as the bundle is (every
# NFSMW_HLE_* off), B with the VAR=value given (say NFSMW_HLE_RESOLVE=1
# NFSMW_HLE_LAZY=1 NFSMW_HLE_PRESENT=1). Run from the repository's root on
# the Deck (scripts/timedemo.sh, scripts/bench_compare.py,
# scripts/compare_shots.py), the race at ~/sb-bench/race.jlg, runs into
# ~/sb-bench/runs/<label>A-<n>, <label>B-<n>.
#
# At 5 W a run's fps varies by about 10% and drifts with the heat, so the
# runs alternate, A B, then B A, and so on (each side as often first): only
# such pairs compare. Each run logs the APU's power cap (amdgpu's hwmon
# power1_cap, in uW) before and after: a run whose cap isn't the first run's
# (the TDP slider moved) is reported, and the comparison is to be redone.
#
# First, one capped pair with pictures (NFSMW_BENCH_SHOTS, measuring
# nothing): compare_shots.py --strict, which must pass. Then the pairs,
# uncapped (NFSMW_TIMEDEMO_UNCAPPED=1), and bench_compare.py A -- B: the gate is
# the GPU's busy time a frame, B lower by 1.5 ms or more (and outside the
# noise) for a go.
set -u
[ $# -ge 3 ] || { echo "usage: $0 <bundle> <pairs> <label> [VAR=value...]" >&2; exit 2; }
BUNDLE="$1"; PAIRS="$2"; LABEL="$3"; shift 3
B=("$@")
OUT="$HOME/sb-bench/runs"
SHOTS=NFSMW_BENCH_SHOTS=1600,2200,2800,3400,4000,4600,5200
cap() {
    local f
    for f in /sys/class/drm/card*/device/hwmon/hwmon*/power1_cap; do
        [ -r "$f" ] && { cat "$f"; return; }
    done
    echo unknown
}
FIRST_CAP="$(cap)"
echo "[deck_ab] power cap at the start: $FIRST_CAP uW; A: the bundle as it is; B: ${B[*]}"
LOG="$OUT/$LABEL-caps.txt"
mkdir -p "$OUT"
: > "$LOG"
run() {  # run <side> <name> [VAR=value...]
    local side="$1" name="$2"; shift 2
    local before after
    before="$(cap)"
    rm -rf "${OUT:?}/$name-1"
    scripts/timedemo.sh run "$name" 1 --bundle "$BUNDLE" --out "$OUT" "$@"
    after="$(cap)"
    echo "$name $side cap_before=$before cap_after=$after" >> "$LOG"
    if [ "$before" != "$FIRST_CAP" ] || [ "$after" != "$FIRST_CAP" ]; then
        echo "[deck_ab] $name: the power cap moved ($before -> $after, first $FIRST_CAP): redo this comparison" >&2
    fi
}
# The pictures (capped, as the warm-ups).
run A "${LABEL}WA" NFSMW_TIMEDEMO_UNCAPPED=0 NFSMW_BENCH_FROM=1530 "$SHOTS"
run B "${LABEL}WB" NFSMW_TIMEDEMO_UNCAPPED=0 NFSMW_BENCH_FROM=1530 "$SHOTS" "${B[@]}"
python3 scripts/compare_shots.py "$OUT/${LABEL}WA-1" "$OUT/${LABEL}WB-1" --strict --diff "$OUT/${LABEL}-shots-diff"
echo "[deck_ab] compare_shots --strict: exit $?"
# The measured pairs.
for i in $(seq 1 "$PAIRS"); do
    if [ $((i % 2)) -eq 1 ]; then
        run A "${LABEL}A$i" NFSMW_TIMEDEMO_UNCAPPED=1 NFSMW_BENCH_FROM=1530
        run B "${LABEL}B$i" NFSMW_TIMEDEMO_UNCAPPED=1 NFSMW_BENCH_FROM=1530 "${B[@]}"
    else
        run B "${LABEL}B$i" NFSMW_TIMEDEMO_UNCAPPED=1 NFSMW_BENCH_FROM=1530 "${B[@]}"
        run A "${LABEL}A$i" NFSMW_TIMEDEMO_UNCAPPED=1 NFSMW_BENCH_FROM=1530
    fi
done
A_RUNS=(); B_RUNS=()
for i in $(seq 1 "$PAIRS"); do A_RUNS+=("$OUT/${LABEL}A$i-1"); B_RUNS+=("$OUT/${LABEL}B$i-1"); done
python3 scripts/bench_compare.py "${A_RUNS[@]}" -- "${B_RUNS[@]}"
echo "[deck_ab] power caps: $LOG"
cat "$LOG"
# GPU busy a frame per run, in pair order (the gate: B lower by >= 1.5 ms).
python3 - "$OUT" "$LABEL" "$PAIRS" <<'EOF'
import json, sys
out, label, pairs = sys.argv[1], sys.argv[2], int(sys.argv[3])
def busy(name):  # summary.json's gpu.busy_mean: ms a swap
    try:
        return float(json.load(open(f'{out}/{name}-1/summary.json'))['gpu']['busy_mean'])
    except (OSError, KeyError, ValueError):
        return float('nan')
d = []
for i in range(1, pairs + 1):
    a, b = busy(f'{label}A{i}'), busy(f'{label}B{i}')
    d.append(a - b)
    print(f'pair {i}: GPU busy A {a:.2f} ms, B {b:.2f} ms, A - B {a - b:+.2f} ms')
d.sort()
print(f'median A - B: {d[len(d) // 2]:+.2f} ms a frame (go: >= +1.50 and compare_shots --strict passing)')
EOF
