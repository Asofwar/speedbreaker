#!/bin/bash
# sharewin.sh <bundle> [seconds]: waits for the bundle's timedemo window, then
# samples the per-process GPU share for <seconds> (default 12) and prints it.
B="${1:-HLEC}"; S="${2:-12}"
L="$HOME/Games/SpeedBreaker-dev/$B/last-run.log"
for i in $(seq 1 240); do grep -q "window began" "$L" 2>/dev/null && break; sleep 1; done
sleep 5
bash "$HOME/Games/SpeedBreaker-dev/HLEkit/gpushare2.sh" "$S" | sort -u -k1,1
