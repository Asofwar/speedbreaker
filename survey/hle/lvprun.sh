#!/bin/bash
# lvprun.sh <label> [VAR=value...]: one rendered replay of /work/race/race.jlg
# on the CPU: Mesa's lavapipe (pacman -S vulkan-swrast) under Xvfb
# (xorg-server-xvfb), in the sb-build container, which has no GPU. It renders
# what the Deck renders, slowly (about 5 frames a second), so its pictures
# (NFSMW_BENCH_SHOTS, compared with compare_shots.py) and the renderer's own
# counters can be checked off the Deck. Its GPU times mean nothing for the
# Deck.
# - The game: $SB_BUILD/runtime/SpeedBreaker (default /work/hle/build). A
#   run lasts about half an hour: give it a copy (snap.sh), so that a build
#   meanwhile doesn't change the binary under it.
# - Guest memory in shadow mode, as on the Deck (RADV refuses the import):
#   imported, lavapipe's own threads would touch write-watched pages and wait
#   for the GPU, that is for themselves.
# - No submits to a starving GPU (NFSMW_STARVE_SUBMIT_US): lavapipe always
#   starves, and they would split passes a Deck at 5 W keeps whole (its
#   queue never empties). Counts of passes and barriers then compare.
# Report in /work/hle/runs/<label>-1/, HOME /work/hle/home/<label>, a display
# of its own, so several can run at once.
set -u
LABEL="$1"; shift
BUILD="${SB_BUILD:-/work/hle/build}"
D=$(( 100 + $(echo "$LABEL" | cksum | cut -d' ' -f1) % 800 ))
Xvfb ":$D" -screen 0 1280x720x24 -nolisten tcp > "/tmp/xvfb-$LABEL.log" 2>&1 &
XPID=$!
trap 'kill $XPID 2>/dev/null' EXIT
sleep 1
rm -rf "/work/hle/home/$LABEL" "/work/hle/runs/$LABEL-1"
mkdir -p "/work/hle/home/$LABEL" /work/hle/runs
cd /work/hle/src || exit 2
HOME="/work/hle/home/$LABEL" DISPLAY=":$D" VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
    bash scripts/timedemo.sh run "$LABEL" 1 --build "$BUILD" \
    --race /work/race/race.jlg --out /work/hle/runs --timeout 14400 \
    NFSMW_ALLOW_MODIFIED_FILES=1 SDL_AUDIODRIVER=dummy NFSMW_BENCH_FROM=1530 NFSMW_SHARED_MEMORY=shadow \
    NFSMW_STARVE_SUBMIT_US=100000000 NFSMW_BENCH_SHOTS=1600,2200,2800,3400,4000,4600,5200 "$@"
