#!/bin/bash
# The PGO training build's exit hook (runtime/debug/pgo_exit.c, linked with
# --wrap=_Exit by CMakeLists.txt when SB_PGO_GENERATE is on): a program
# compiled with -fprofile-generate that leaves through _Exit, as the game
# always does, writes its counts with the hook, and none without it (the
# reason for the hook). With llvm-profdata beside the compiler or on PATH,
# the counts must also merge into a profile that has the program's function.
# Registered by tests/CMakeLists.txt, or:
#   bash tests/pgo_exit_test.sh clang runtime/debug/pgo_exit.c
# Exit 77 (skipped) when the compiler can't build an instrumented program at
# all (no LLVM profile runtime installed).
set -u
CC=${1:?usage: pgo_exit_test.sh <clang> <pgo_exit.c>}
HOOK=${2:?usage: pgo_exit_test.sh <clang> <pgo_exit.c>}
[ -f "$HOOK" ] || { echo "FAIL: no $HOOK"; exit 1; }

T="$(mktemp -d "${TMPDIR:-/tmp}/pgo_exit_test.XXXXXX")" || exit 1
trap 'rm -rf "$T"' EXIT
# count(100) is 25 * (0 + 1 + 2 + 3) = 150: exit code 3, through _Exit.
# Not inlined, so that the profile has it by name.
cat > "$T/main.c" << 'EOF'
#include <stdlib.h>
__attribute__((noinline)) static int count(int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        s += i & 3;
    return s;
}
int main(int argc, char** argv)
{
    (void)argv;
    _Exit(count(argc * 100) == 150 ? 3 : 4);
}
EOF
if ! "$CC" -O1 -fprofile-generate "$T/main.c" -o "$T/plain" 2> "$T/cc.log"; then
    echo "SKIP: $CC can't build an instrumented program (no LLVM profile runtime?):"
    head -3 "$T/cc.log"
    exit 77
fi
if ! "$CC" -O1 -fprofile-generate "$T/main.c" "$HOOK" -Wl,--wrap=_Exit -o "$T/hooked" 2> "$T/cc.log"; then
    echo "FAIL: the program with the hook doesn't build:"
    head -5 "$T/cc.log"
    exit 1
fi

fail=0
# run <program> <folder>: the program with LLVM_PROFILE_FILE in <folder>; sets CODE.
run() {
    mkdir -p "$2"
    LLVM_PROFILE_FILE="$2/%p.profraw" "$1"
    CODE=$?
}
run "$T/plain" "$T/plain.out"
[ "$CODE" = 3 ] || { echo "FAIL: without the hook the program exited $CODE, not 3"; fail=1; }
# The profile runtime makes the file when the program starts and fills it at
# exit(): after _Exit it stays empty.
if [ -n "$(find "$T/plain.out" -name '*.profraw' -size +0c)" ]; then
    # Not a failure: this LLVM writes counts on _Exit by itself, and the
    # hook only writes them a second time.
    echo "note: without the hook, $CC's profile runtime wrote counts on _Exit too"
else
    echo "ok: without the hook, _Exit left no counts"
fi
run "$T/hooked" "$T/hooked.out"
[ "$CODE" = 3 ] || { echo "FAIL: with the hook the program exited $CODE, not 3"; fail=1; }
raw="$(compgen -G "$T/hooked.out/*.profraw" | head -1)"
if [ -z "$raw" ] || [ ! -s "$raw" ]; then
    echo "FAIL: with the hook, _Exit wrote no counts"
    fail=1
else
    echo "ok: _Exit wrote the counts ($(wc -c < "$raw") bytes)"
    PROFDATA="$(dirname "$(command -v "$CC")")/llvm-profdata"
    [ -x "$PROFDATA" ] || PROFDATA="$(command -v llvm-profdata || true)"
    if [ -n "$PROFDATA" ] && [ -x "$PROFDATA" ]; then
        if ! "$PROFDATA" merge -o "$T/p.profdata" "$raw" 2> "$T/merge.log"; then
            # Another LLVM release than the compiler's can't read them:
            # nothing this test can tell about the hook.
            echo "note: $PROFDATA couldn't merge them (another LLVM release than $CC's?): $(head -1 "$T/merge.log")"
        # A file-local function is named <file>;count (<file>:count before LLVM 18).
        elif "$PROFDATA" show --all-functions "$T/p.profdata" 2> /dev/null | grep -qE "[;:]count:$"; then
            echo "ok: $PROFDATA merged them, with the program's count()"
        else
            echo "FAIL: the merged counts have no count()"
            fail=1
        fi
    fi
fi
[ "$fail" = 0 ] && echo "pgo_exit_test: ok"
exit "$fail"
