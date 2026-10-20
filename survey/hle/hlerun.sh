#!/bin/bash
# hlerun.sh <label> [VAR=value...]: one headless replay of /work/race/race.jlg
# with the study build in /work/hle/build (D3D and command processor
# counters on), reports in /work/hle/runs/<label>-1/, counters in
# /work/hle/runs/<label>.prof. HOME is /work/hle/home/<label>.
set -u
LABEL="$1"; shift
mkdir -p "/work/hle/home/$LABEL" /work/hle/runs
rm -f "/work/hle/runs/$LABEL.prof"
cd /work/hle/src || exit 2
HOME="/work/hle/home/$LABEL" exec bash scripts/timedemo.sh run "$LABEL" 1 --build /work/hle/build \
    --race /work/race/race.jlg --out /work/hle/runs --timeout 1200 NFSMW_HEADLESS=1 \
    NFSMW_ALLOW_MODIFIED_FILES=1 SDL_AUDIODRIVER=dummy NFSMW_BENCH_FROM=1530 \
    D3DPROF=1 D3DPROF_DUMP=4500,5000,6000,7000,8000,8100 D3DPROF_OUT="/work/hle/runs/$LABEL.prof" "$@"
