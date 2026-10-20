#!/bin/bash
# hldet.sh <label> <n> [VAR=value...]: n headless replays of /work/race/race.jlg
# at once with the study game in $SB_BUILD (default /work/hle/build), each
# with a HOME of its own (/work/hle/home/<label><i>), reports in
# /work/hle/runs/<label><i>-1/. Then each run's status and window trace_hash
# (the game's loop, step and clock of every frame: the same in every run, and
# as on the Deck, while nothing changes the game's behaviour). Headless skips
# the renderer; the D3D hooks (NFSMW_HLE_RESOLVE) still run.
set -u
LABEL="$1"; N="$2"; shift 2
BUILD="${SB_BUILD:-/work/hle/build}"
cd /work/hle/src || exit 2
for i in $(seq 1 "$N"); do
    rm -rf "/work/hle/home/$LABEL$i" "/work/hle/runs/$LABEL$i-1"
    mkdir -p "/work/hle/home/$LABEL$i"
    HOME="/work/hle/home/$LABEL$i" bash scripts/timedemo.sh run "$LABEL$i" 1 --build "$BUILD" \
        --race /work/race/race.jlg --out /work/hle/runs --timeout 3600 NFSMW_HEADLESS=1 \
        NFSMW_ALLOW_MODIFIED_FILES=1 SDL_AUDIODRIVER=dummy NFSMW_BENCH_FROM=1530 "$@" > "/work/hle/$LABEL$i.out" 2>&1 &
done
wait
for i in $(seq 1 "$N"); do
    python3 -c "import json,sys; s=json.load(open(sys.argv[1])); print(sys.argv[2], s['status'], s['determinism']['trace_hash'], s['window']['frames'])" \
        "/work/hle/runs/$LABEL$i-1/summary.json" "$LABEL$i" 2>/dev/null || echo "$LABEL$i no report"
done
