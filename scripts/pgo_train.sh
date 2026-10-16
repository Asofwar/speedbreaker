#!/usr/bin/env bash
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
# Profile-guided optimisation (PGO, Linux): the game's code compiled with the
# counts of a training run, so that the compiler lays out, inlines and
# predicts by how often each branch, call and loop really ran instead of by
# its own guesses. Optional: a build without -DSB_PGO_PROFILE is the usual
# one (CMakeLists.txt, docs/timedemo.md "Faster builds"). Three steps, with
# the clang and llvm-profdata of one LLVM release:
#
#   scripts/pgo_train.sh build [<folder>] [cmake options...]
#       Configures and builds the instrumented game in <folder> (default
#       build/pgo-gen): -DSB_PGO_GENERATE=ON. It plays as the game does,
#       a little slower, and counts as it runs.
#   scripts/pgo_train.sh run <label> [timedemo.sh run options] [VAR=value...]
#       One timedemo replay of the instrumented game (timedemo.sh run <label>
#       with these options: --build <folder> for the dev build, --bundle <dir>
#       for a SteamOS bundle of it), which writes the counts when the game
#       exits: <pid>.profraw in PGO_RAW_DIR (default ~/sb-bench/pgo/raw).
#       The race is the training: replay it as it is measured, e.g.
#       NFSMW_TIMEDEMO_UNCAPPED=1 NFSMW_BENCH_FROM=1530. Several runs, or
#       several races, can be merged into one profile.
#   scripts/pgo_train.sh merge <profile.profdata> <.profraw file or folder>...
#       Merges the counts into the profile (llvm-profdata merge;
#       LLVM_PROFDATA names another llvm-profdata).
#
# After the runs, the profile, and the optimised build in a folder of its own,
# from the same checkout path as the instrumented one (the profile names the
# runtime's file-local functions by their source files' paths), best with
# ThinLTO as well (-DSB_LTO=ON):
#
#   scripts/pgo_train.sh merge build/speedbreaker.profdata ~/sb-bench/pgo/raw
#   cmake -S . -B build/pgo -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
#       -DCMAKE_CXX_COMPILER=clang++ -DSB_LTO=ON -DSB_PGO_PROFILE=$PWD/build/speedbreaker.profdata
#   ninja -C build/pgo SpeedBreaker
#
# A profile stays usable while the code changes: the compiler matches it
# function by function and leaves out what changed since the training (one
# warning a file, -Wprofile-instr-out-of-date). Train again when much has.
# Code the training never ran (police chases, when the race has none) is
# compiled as rarely used: small rather than fast.
#
# For a SteamOS bundle, build in the container (README.md, "Building from
# source"), bundle the instrumented folder with SB_ALLOW_PATHS=1
# (scripts/steamos/make_bundle.sh build/pgo-gen <out>: the instrumented game
# carries its functions' names, source paths included), and run this script's
# run step on the Steam Deck with --bundle, beside timedemo.sh (it calls the
# timedemo.sh in its own folder); then copy the .profraw files back to merge
# them where the build is.
set -euo pipefail
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
die() { echo "pgo_train.sh: $*" >&2; exit 2; }
usage() {
    sed -n '2,/^[^#]/{/^#/s/^# \{0,1\}//p}' "$0" >&2
    exit 2
}

[ $# -ge 1 ] || usage
STEP="$1"
shift
case "$STEP" in
    build)
        ROOT="$(cd "$HERE/.." && pwd)"
        [ -f "$ROOT/CMakeLists.txt" ] || die "build runs from the repository's scripts/ folder"
        FOLDER="build/pgo-gen"
        if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then FOLDER="$1"; shift; fi
        cd "$ROOT"
        cmake -S . -B "$FOLDER" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
            -DCMAKE_CXX_COMPILER=clang++ -DSB_PGO_GENERATE=ON "$@"
        ninja -C "$FOLDER" SpeedBreaker
        echo "pgo_train.sh: the instrumented game is $FOLDER/runtime/SpeedBreaker; next: scripts/pgo_train.sh run <label> --build $FOLDER ..."
        ;;
    run)
        [ $# -ge 1 ] || usage
        [ -x "$HERE/timedemo.sh" ] || die "no timedemo.sh beside this script ($HERE)"
        RAW="${PGO_RAW_DIR:-$HOME/sb-bench/pgo/raw}"
        case "$RAW" in /*) ;; *) RAW="$PWD/$RAW" ;; esac
        mkdir -p "$RAW" || die "can't make $RAW"
        marker="$(mktemp)"
        trap 'rm -f "$marker"' EXIT
        code=0
        # %p: the game's process ID, a file per run.
        "$HERE/timedemo.sh" run "$@" LLVM_PROFILE_FILE="$RAW/%p.profraw" || code=$?
        written="$(find "$RAW" -maxdepth 1 -name '*.profraw' -newer "$marker" 2>/dev/null)"
        [ -n "$written" ] || die "the run wrote no .profraw in $RAW: is it the instrumented build (scripts/pgo_train.sh build)?"
        echo "pgo_train.sh: counts written: $written"
        # A replay that didn't end ok (diverged, timed out) still counted
        # what it ran, most of the game's work but perhaps not the race: the
        # file is kept, and the exit code says so.
        exit "$code"
        ;;
    merge)
        [ $# -ge 2 ] || usage
        OUTPUT="$1"
        shift
        PROFDATA="${LLVM_PROFDATA:-llvm-profdata}"
        command -v "$PROFDATA" > /dev/null || die "no $PROFDATA (LLVM's; LLVM_PROFDATA=<path> names one)"
        inputs=()
        for p in "$@"; do
            if [ -d "$p" ]; then
                for f in "$p"/*.profraw; do [ -f "$f" ] && inputs+=("$f"); done
            elif [ -f "$p" ]; then
                inputs+=("$p")
            else
                die "no file or folder $p"
            fi
        done
        [ ${#inputs[@]} -gt 0 ] || die "no .profraw files in $*"
        mkdir -p "$(dirname "$OUTPUT")"
        "$PROFDATA" merge -o "$OUTPUT" "${inputs[@]}"
        OUTPUT="$(cd "$(dirname "$OUTPUT")" && pwd)/$(basename "$OUTPUT")"
        echo "pgo_train.sh: $OUTPUT from ${#inputs[@]} run(s); next: configure with -DSB_PGO_PROFILE=$OUTPUT"
        ;;
    *) usage ;;
esac
