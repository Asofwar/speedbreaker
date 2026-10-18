#!/bin/bash
# snap.sh <tag>: a copy of the study build's game, /work/hle/bin/<tag>/runtime/
# SpeedBreaker, for runs that must not see a later build (SB_BUILD=/work/hle/bin/<tag>).
set -eu
mkdir -p "/work/hle/bin/$1/runtime"
cp /work/hle/build/runtime/SpeedBreaker "/work/hle/bin/$1/runtime/SpeedBreaker"
echo "/work/hle/bin/$1"
