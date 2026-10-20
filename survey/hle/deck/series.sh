#!/bin/bash
# series.sh <bundle> <rounds> <label:VAR=v,VAR=v>...: profrun2 runs over the
# variants in turn, <rounds> times through (A B C A B C), a line each.
B="$1"; R="$2"; shift 2
K=$HOME/Games/SpeedBreaker-dev/HLEkit
for round in $(seq 1 "$R"); do
    for spec in "$@"; do
        label="${spec%%:*}-$round"; vars="${spec#*:}"
        bash "$K/profrun2.sh" "$B" "$label" ${vars//,/ } > "$HOME/sb-bench/$label.sum.txt" 2>&1
        echo "== $label ($vars)"; grep -E "power1_cap|fps |power:|CPU ms" "$HOME/sb-bench/$label.sum.txt" | cut -c1-200
        grep -h "queue empty" "$HOME/Games/SpeedBreaker-dev/$B/last-run.log" | tail -1 | grep -o "queue empty [0-9]*%"
    done
done
