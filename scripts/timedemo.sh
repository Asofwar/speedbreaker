#!/bin/bash
# Timedemo runs (docs/timedemo.md): a race recorded once through the game's
# own record/replay system (EA's Joylog), then replayed as a benchmark that
# drives the same race frame for frame, ends by itself and leaves a report.
#
#   scripts/timedemo.sh capture <label> [options] [VAR=value...]
#   scripts/timedemo.sh run <label> [runs] [options] [VAR=value...]
#
# capture: the game records <race> (and writes <race>.json beside it) while
#   you drive, or --input-script drives the menus; the profile it starts
#   from is copied first to <race>.save/ (save/ and profile_settings.bin;
#   the game snapshots it there too), since a replay must start from the
#   very same one. The game plays on a copy of it; that copy and the log
#   stay in <out>/capture/<label>/.
# run: replays <race> [runs] times (1), each from a fresh copy of that
#   snapshot (a replay can autosave: the snapshot itself stays as captured),
#   each into <out>/<label>-<n>/ with the next free n (so separate calls add
#   up: A-1 B-1 A-2 B-2 ...), waits for the game to exit by itself, and
#   prints each run's exit code and report. Then compare:
#   scripts/bench_compare.py <out>/A-[0-9]* -- <out>/B-[0-9]*
#
# Options:
#   --bundle <dir>       the game of a SteamOS bundle (<dir>/speedbreaker.sh);
#                        default: the dev build, build/main/runtime/SpeedBreaker
#   --build <dir>        the dev build of another build folder,
#                        <dir>/runtime/SpeedBreaker (still run from the repo
#                        root; scripts/pgo_train.sh's instrumented one)
#   --race <path.jlg>    the race (default ~/sb-bench/race.jlg)
#   --out <dir>          where runs go (default ~/sb-bench/runs)
#   --timeout <s>        stop a game still running after this long (run:
#                        NFSMW_BENCH_TIMEOUT + 300; capture: --seconds + 600,
#                        or never); 0: never
#   capture only:
#   --seconds <n>        stop n s after the race has loaded
#                        (NFSMW_TIMEDEMO_CAPTURE_SECONDS; default: at the quit)
#   --input-script <s>   NFSMW_INPUT_SCRIPT: pad taps by seconds from launch
#   --save-from <dir>    the profile to record with: a data folder holding
#                        save/ and profile_settings.bin (default: the game's
#                        own; a portable bundle's data/)
#   --fresh-save         record with a new, empty profile instead
#   --force              replace a race already at <race>
#   run only:
#   --warmup             one replay first that doesn't count (pipeline cache,
#                        clocks, file cache), its report in <out>/warmup/<label>/
#   VAR=value            more environment for the game, after the fixed one
#                        below (NFSMW_TIMEDEMO_UNCAPPED=1, NFSMW_BENCH_STEPS=3600)
#
# Every run gets NFSMW_FRAME_RATE=60 NFSMW_MUTE=1 NFSMW_PERF_OVERLAY=0
# NFSMW_INPUT_LOG=0 NFSMW_LOG_LATENCY=0: the same pacing in every run, and
# nothing on screen or in the log that the comparison would measure.
# Over SSH (no DISPLAY), the game goes to the Steam Deck's screen: the
# Desktop Mode session's display (from plasmashell), else Game Mode's
# (gamescope's Xwayland, :0).
# Exit: 0 every run ended ok with a report, 1 one didn't, 2 bad arguments.

ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"

say() { echo "[timedemo] $*"; }
die() { echo "[timedemo] $*" >&2; exit 2; }
usage() {
    sed -n '2,/^[^#]/{/^#/s/^# \{0,1\}//p}' "$0" >&2
    exit 2
}

[ $# -ge 2 ] || usage
MODE="$1" LABEL="$2"
shift 2
case "$MODE" in capture|run) ;; *) usage ;; esac
# The label names folders and the report: nothing a path or a shell would read.
case "$LABEL" in
    ""|-*|*[!A-Za-z0-9._-]*) die "a label is letters, digits, '.', '_' and '-' (it names folders): \"$LABEL\"" ;;
esac
RUNS=1
if [ "$MODE" = run ] && [ $# -gt 0 ]; then
    case "$1" in ""|*[!0-9]*) ;; *) RUNS="$1"; shift ;; esac
fi

BUNDLE="" BUILD="" RACE="$HOME/sb-bench/race.jlg" OUT="$HOME/sb-bench/runs" TIMEOUT=""
CAPTURE_SECONDS="" INPUT_SCRIPT="" SAVE_FROM="" FRESH_SAVE=0 FORCE=0 WARMUP=0
EXTRA=()
CAPTURE_ONLY="" RUN_ONLY=""
need() { [ -n "${2:-}" ] || die "$1 needs a value"; }
while [ $# -gt 0 ]; do
    case "$1" in
        --bundle) need "$@"; BUNDLE="$2"; shift 2 ;;
        --build) need "$@"; BUILD="$2"; shift 2 ;;
        --race) need "$@"; RACE="$2"; shift 2 ;;
        --out) need "$@"; OUT="$2"; shift 2 ;;
        --timeout) need "$@"; TIMEOUT="$2"; shift 2 ;;
        --seconds) need "$@"; CAPTURE_SECONDS="$2"; CAPTURE_ONLY="$1"; shift 2 ;;
        --input-script) need "$@"; INPUT_SCRIPT="$2"; CAPTURE_ONLY="$1"; shift 2 ;;
        --save-from) need "$@"; SAVE_FROM="$2"; CAPTURE_ONLY="$1"; shift 2 ;;
        --fresh-save) FRESH_SAVE=1; CAPTURE_ONLY="$1"; shift ;;
        --force) FORCE=1; CAPTURE_ONLY="$1"; shift ;;
        --warmup) WARMUP=1; RUN_ONLY="$1"; shift ;;
        -h|--help) usage ;;
        [A-Za-z_]*=*)
            case "${1%%=*}" in *[!A-Za-z0-9_]*) die "neither an option nor VAR=value: $1" ;; esac
            EXTRA+=("$1")
            shift
            ;;
        *) die "unknown argument: $1 (scripts/timedemo.sh alone lists the options)" ;;
    esac
done
[ "$MODE" = run ] && [ -n "$CAPTURE_ONLY" ] && die "$CAPTURE_ONLY is for capture"
[ "$MODE" = capture ] && [ -n "$RUN_ONLY" ] && die "$RUN_ONLY is for run"
[ "$RUNS" -ge 1 ] || die "runs: 1 or more"
for n in "$TIMEOUT" "$CAPTURE_SECONDS"; do
    case "$n" in *[!0-9]*) die "not a number of seconds: $n" ;; esac
done
[ "$FRESH_SAVE" = 1 ] && [ -n "$SAVE_FROM" ] && die "--fresh-save or --save-from, not both"
[ -n "$BUNDLE" ] && [ -n "$BUILD" ] && die "--bundle or --build, not both"

# Absolute paths: the game runs from another folder (the dev build from the
# repo root) and NFSMW_SAVE_DIR is only taken when absolute.
abspath() { case "$1" in /*) printf '%s\n' "$1" ;; *) printf '%s\n' "$PWD/$1" ;; esac; }
RACE="$(abspath "$RACE")"
OUT="$(abspath "$OUT")"
case "$RACE" in *.jlg) ;; *) die "--race wants a .jlg file: $RACE" ;; esac

if [ -n "$BUNDLE" ]; then
    BUNDLE="$(cd "$BUNDLE" 2>/dev/null && pwd)" || die "no folder $BUNDLE"
    [ -x "$BUNDLE/speedbreaker.sh" ] || die "no speedbreaker.sh in $BUNDLE: --bundle wants an unpacked SteamOS download"
    say "game: the bundle in $BUNDLE ($(cat "$BUNDLE/VERSION" 2>/dev/null || echo "no VERSION file"))"
else
    # The dev build: build/main's, or that of the folder --build names.
    GAME="$ROOT/build/main/runtime/SpeedBreaker"
    [ -n "$BUILD" ] && GAME="$(abspath "$BUILD")/runtime/SpeedBreaker"
    [ -x "$GAME" ] || die "no dev build at $GAME: build it, or --bundle <dir>"
    say "game: the dev build, $GAME"
fi

# borrow_env <pid> <name>...: those variables from another process of this
# user's (its /proc/<pid>/environ), exported here.
borrow_env() {
    local pid="$1" entry want
    shift
    [ -r "/proc/$pid/environ" ] || return 1
    while IFS= read -r -d '' entry; do
        for want in "$@"; do
            [ "${entry%%=*}" = "$want" ] && export "${entry?}"
        done
    done < "/proc/$pid/environ"
    return 0
}

# Over SSH (the usual way into a Steam Deck) there is no display here: the
# game borrows the session's on the Deck's screen. Desktop Mode is
# plasmashell's (its X display and the bus the desktop's programs use);
# Game Mode is gamescope's Xwayland, as Steam itself sees it (its own
# environment), else gamescope's usual :0 and gamescope-0. SDL then uses X11,
# as both launchers have it.
if [ -z "${DISPLAY:-}" ] && [ -d /proc ] && command -v pgrep > /dev/null; then
    uid="$(id -u)"
    if pid="$(pgrep -n -u "$uid" -x plasmashell)"; then
        borrow_env "$pid" DISPLAY XAUTHORITY XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS
        say "display: Desktop Mode's (plasmashell $pid): DISPLAY=${DISPLAY:-none}"
    elif pid="$(pgrep -n -u "$uid" -x 'gamescope(-wl)?')"; then
        if steam="$(pgrep -n -u "$uid" -x steam)"; then
            borrow_env "$steam" DISPLAY GAMESCOPE_WAYLAND_DISPLAY XAUTHORITY XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS
        fi
        export DISPLAY="${DISPLAY:-:0}" GAMESCOPE_WAYLAND_DISPLAY="${GAMESCOPE_WAYLAND_DISPLAY:-gamescope-0}"
        export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$uid}"
        say "display: Game Mode's (gamescope $pid): DISPLAY=$DISPLAY, GAMESCOPE_WAYLAND_DISPLAY=$GAMESCOPE_WAYLAND_DISPLAY" \
            "(a game not started by Steam may stay behind Steam's own screen: docs/timedemo.md)"
    else
        say "warning: no DISPLAY, and no Desktop Mode or Game Mode session of this user's to show the game in"
    fi
fi

FIXED_ENV=(NFSMW_FRAME_RATE=60 NFSMW_MUTE=1 NFSMW_PERF_OVERLAY=0 NFSMW_INPUT_LOG=0 NFSMW_LOG_LATENCY=0)
# The dev build has no launcher: what the SteamOS ones set for Game Mode.
if [ -z "$BUNDLE" ] && [ -n "${GAMESCOPE_WAYLAND_DISPLAY:-}" ]; then
    FIXED_ENV+=(NFSMW_GAME_MODE="${NFSMW_GAME_MODE:-1}" SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}")
fi

# NFSMW_* already in this shell apply to every run as well (where the script
# sets one itself, its value wins): named once, since a leftover one
# (NFSMW_ASPECT, NFSMW_GPU_TIMING) changes what is measured.
inherited=""
while IFS= read -r name; do
    case "$name" in NFSMW_*) inherited="$inherited $name=${!name}" ;; esac
done < <(compgen -e)
if [ -n "$inherited" ]; then say "from this shell:$inherited"; fi
if [ ${#EXTRA[@]} -gt 0 ]; then say "given: ${EXTRA[*]}"; fi

# extra_value <NAME>: its value among the VAR=value arguments, else the shell's.
extra_value() {
    local arg value="${!1:-}"
    for arg in "${EXTRA[@]}"; do
        [ "${arg%%=*}" = "$1" ] && value="${arg#*=}"
    done
    printf '%s\n' "$value"
}

GAME_PID=""
# stop_game: SIGTERM, then SIGKILL 15 s later.
stop_game() {
    [ -n "$GAME_PID" ] || return 0
    kill -TERM "$GAME_PID" 2>/dev/null
    for _ in $(seq 15); do
        kill -0 "$GAME_PID" 2>/dev/null || return 0
        sleep 1
    done
    kill -KILL "$GAME_PID" 2>/dev/null
}
# A game started with & in a script ignores the terminal's Ctrl+C: stop it here.
trap 'say "interrupted: stopping the game"; stop_game; exit 130' INT TERM HUP

# run_game <folder> <timeout s, 0 never> VAR=value...: the game with the fixed
# environment, then these, until it exits by itself; its log to
# <folder>/game.log, its exit code in GAME_CODE (124: stopped at the timeout).
run_game() {
    local dir="$1" timeout="$2" start log candidate
    shift 2
    : > "$dir/.started"
    if [ -n "$BUNDLE" ]; then
        # The launcher keeps its own log (last-run.log, in the bundle or the
        # data folder): this file only holds what comes before it opens that.
        env "${FIXED_ENV[@]}" "$@" "${EXTRA[@]}" "$BUNDLE/speedbreaker.sh" > "$dir/launcher.log" 2>&1 < /dev/null &
    else
        # From the repo root, where the dev build finds game/files.
        (cd "$ROOT" && exec env "${FIXED_ENV[@]}" "$@" "${EXTRA[@]}" "$GAME") > "$dir/game.log" 2>&1 < /dev/null &
    fi
    GAME_PID=$!
    start=$(date +%s)
    local timed_out=0
    while kill -0 "$GAME_PID" 2>/dev/null; do
        if [ "$timeout" -gt 0 ] && [ $(( $(date +%s) - start )) -ge "$timeout" ]; then
            say "still running after ${timeout} s: stopping it"
            stop_game
            timed_out=1
            break
        fi
        sleep 1
    done
    # (Quietly: the exit code below says how it ended, signals included.)
    wait "$GAME_PID" 2>/dev/null
    GAME_CODE=$?
    GAME_PID=""
    [ "$timed_out" = 1 ] && GAME_CODE=124
    if [ -n "$BUNDLE" ]; then
        log=""
        for candidate in "$BUNDLE/last-run.log" "${XDG_DATA_HOME:-$HOME/.local/share}/speedbreaker/last-run.log" \
            "${XDG_RUNTIME_DIR:-/nonexistent}/speedbreaker/last-run.log"; do
            if [ -f "$candidate" ] && [ "$candidate" -nt "$dir/.started" ]; then
                log="$candidate"
                break
            fi
        done
        if [ -n "$log" ]; then
            cp -f "$log" "$dir/game.log"
            [ -s "$dir/launcher.log" ] || rm -f "$dir/launcher.log"
        else
            mv -f "$dir/launcher.log" "$dir/game.log"
        fi
    fi
    rm -f "$dir/.started"
    say "game exited after $(( $(date +%s) - start )) s: $(code_meaning "$GAME_CODE")"
}

code_meaning() {
    case "$1" in
        0) echo "0, ok" ;;
        2) echo "2, setup failed, the log ended or the bench timed out (the log says which)" ;;
        3) echo "3, diverged: the replay went out of step with the log" ;;
        4) echo "4, interrupted by the runtime's own menu or notice" ;;
        124) echo "stopped at the timeout" ;;
        129|13[0-9]|14[0-9]|15[0-9]) echo "$1, ended by SIG$(kill -l $(($1 - 128)) 2>/dev/null) (a crash? see the log)" ;;
        *) echo "$1 (a crash? see the log)" ;;
    esac
}

if ! mkdir -p "$OUT" 2>/dev/null || [ ! -w "$OUT" ]; then die "can't write to $OUT"; fi

if [ "$MODE" = capture ]; then
    # Where the game itself snapshots the save at a capture's start
    # (game/timedemo.cpp): the script's copy is the same, made first so that
    # a capture never runs on the player's own save.
    SNAPSHOT="$RACE.save"
    WORK="$OUT/capture/$LABEL"
    if [ -e "$RACE" ] || [ -e "$RACE.json" ] || [ -e "$SNAPSHOT" ]; then
        [ "$FORCE" = 1 ] || die "a race is already at $RACE: --force replaces it, or --race <another.jlg>"
        rm -rf "$RACE" "$RACE.json" "$SNAPSHOT"
    fi
    mkdir -p "$(dirname "$RACE")" || die "can't make $(dirname "$RACE")"

    # The profile the race starts from: the snapshot every replay copies.
    if [ -z "$SAVE_FROM" ] && [ "$FRESH_SAVE" = 0 ]; then
        if [ -n "$BUNDLE" ] && [ -f "$BUNDLE/portable.txt" ]; then
            SAVE_FROM="$BUNDLE/data"
        else
            case "${XDG_DATA_HOME:-}" in
                /*) SAVE_FROM="$XDG_DATA_HOME/speedbreaker" ;;
                *) SAVE_FROM="$HOME/.local/share/speedbreaker" ;;
            esac
        fi
    fi
    mkdir -p "$SNAPSHOT" || die "can't make $SNAPSHOT"
    if [ "$FRESH_SAVE" = 1 ]; then
        say "profile: a new, empty one (--fresh-save)"
    else
        [ -d "$SAVE_FROM" ] || die "no folder $SAVE_FROM (--save-from <dir>, or --fresh-save)"
        found=0
        if [ -d "$SAVE_FROM/save" ]; then cp -a "$SAVE_FROM/save" "$SNAPSHOT/" || die "can't copy $SAVE_FROM/save"; found=1; fi
        if [ -f "$SAVE_FROM/profile_settings.bin" ]; then cp -a "$SAVE_FROM/profile_settings.bin" "$SNAPSHOT/" || die "can't copy profile_settings.bin"; found=1; fi
        if [ "$found" = 1 ]; then
            say "profile: from $SAVE_FROM, snapshot in $SNAPSHOT"
        else
            say "profile: $SAVE_FROM has no save/ or profile_settings.bin: recording with a new, empty one"
        fi
    fi
    # The game plays on a copy (<out>/capture/<label>/profile, beside its
    # log): the snapshot stays as the race began.
    rm -rf "$WORK"
    mkdir -p "$WORK/profile" || die "can't make $WORK"
    cp -a "$SNAPSHOT/." "$WORK/profile/"

    timeout="${TIMEOUT:-0}"
    [ -z "$TIMEOUT" ] && [ -n "$CAPTURE_SECONDS" ] && timeout=$((CAPTURE_SECONDS + 600))
    env_args=(NFSMW_TIMEDEMO=capture NFSMW_TIMEDEMO_FILE="$RACE" NFSMW_TIMEDEMO_FIXED=1 NFSMW_SAVE_DIR="$WORK/profile")
    [ -n "$CAPTURE_SECONDS" ] && env_args+=(NFSMW_TIMEDEMO_CAPTURE_SECONDS="$CAPTURE_SECONDS")
    [ -n "$INPUT_SCRIPT" ] && env_args+=(NFSMW_INPUT_SCRIPT="$INPUT_SCRIPT")
    say "capture $LABEL: $RACE${CAPTURE_SECONDS:+, stopping $CAPTURE_SECONDS s after the race has loaded}" \
        "(drive a Quick Race; quit the game to end it${CAPTURE_SECONDS:+ sooner})"
    run_game "$WORK" "$timeout" "${env_args[@]}"
    grep -a '\[timedemo\]' "$WORK/game.log" 2>/dev/null | tail -n 20 | sed 's/^/    /'
    size=0
    [ -f "$RACE" ] && size="$(wc -c < "$RACE" | tr -d ' ')"
    say "log: $WORK/game.log"
    if [ "$GAME_CODE" = 0 ] && [ "$size" -gt 0 ] && [ -f "$RACE.json" ]; then
        say "captured: $RACE ($size bytes), $RACE.json, profile $SNAPSHOT"
        say "replay it: scripts/timedemo.sh run <label> [runs] --race $RACE${BUNDLE:+ --bundle $BUNDLE}"
        exit 0
    fi
    say "capture failed: $(code_meaning "$GAME_CODE"); race file ${size} bytes; $([ -f "$RACE.json" ] && echo "meta written" || echo "no $RACE.json")"
    exit 1
fi

# run
[ -s "$RACE" ] || die "no race at $RACE: scripts/timedemo.sh capture <label> first (or --race <path.jlg>)"
[ -f "$RACE.json" ] || say "warning: no $RACE.json beside the race (its meta: the window's default end, the capture's settings)"
# The profile snapshot: <race>.save/, as the capture left it.
SNAPSHOT="$RACE.save"
[ -d "$SNAPSHOT" ] || die "no profile snapshot at $SNAPSHOT: a replay must start from the profile the race was recorded with" \
    "(make that folder with save/ and profile_settings.bin as they were then, or empty for a new profile)"

bench_timeout="$(extra_value NFSMW_BENCH_TIMEOUT)"
case "$bench_timeout" in ""|*[!0-9]*) bench_timeout=900 ;; esac
timeout="${TIMEOUT:-$((bench_timeout + 300))}"
if [ "$(extra_value NFSMW_BENCH_EXIT)" = 0 ]; then say "warning: NFSMW_BENCH_EXIT=0: the game won't exit after its report, only at the timeout ($timeout s)"; fi

# replay <folder> <label>: one replay from a fresh copy of the snapshot.
replay() {
    local dir="$1" label="$2"
    mkdir -p "$dir/profile"
    cp -a "$SNAPSHOT/." "$dir/profile/"
    say "run $label: $dir"
    run_game "$dir" "$timeout" NFSMW_TIMEDEMO=replay NFSMW_TIMEDEMO_FILE="$RACE" NFSMW_SAVE_DIR="$dir/profile" \
        NFSMW_BENCH_LABEL="$label" NFSMW_BENCH_OUT="$dir"
    grep -a '\[timedemo\] \(DIVERGENCE\|FATAL\)\|\[bench\]' "$dir/game.log" 2>/dev/null | tail -n 5 | sed 's/^/    /'
}

if [ "$WARMUP" = 1 ]; then
    rm -rf "$OUT/warmup/$LABEL"
    mkdir -p "$OUT/warmup/$LABEL"
    replay "$OUT/warmup/$LABEL" "$LABEL-warmup"
fi

results=() failed=0
for _ in $(seq "$RUNS"); do
    n=1
    # mkdir claims the number: two scripts at once never share a folder.
    until mkdir "$OUT/$LABEL-$n" 2>/dev/null; do
        [ -w "$OUT" ] || die "can't write to $OUT"
        n=$((n + 1))
    done
    replay "$OUT/$LABEL-$n" "$LABEL-$n"
    if [ "$GAME_CODE" = 0 ] && [ -f "$OUT/$LABEL-$n/summary.json" ]; then
        results+=("$LABEL-$n: $(code_meaning "$GAME_CODE"): $OUT/$LABEL-$n/summary.json")
    else
        failed=$((failed + 1))
        report="no summary.json"
        [ -f "$OUT/$LABEL-$n/summary.json" ] && report="$OUT/$LABEL-$n/summary.json"
        results+=("$LABEL-$n: $(code_meaning "$GAME_CODE"): $report (log: $OUT/$LABEL-$n/game.log)")
    fi
done

say "done: $((RUNS - failed)) of $RUNS runs ok"
for line in "${results[@]}"; do say "  $line"; done
say "compare: scripts/bench_compare.py $OUT/<A>-[0-9]* -- $OUT/<B>-[0-9]*"
[ "$failed" = 0 ]
