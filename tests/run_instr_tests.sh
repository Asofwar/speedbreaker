#!/usr/bin/env bash
# Instruction tests for our XenonRecomp branch.
#
#   tests/run_instr_tests.sh [test-name ...]
#
# Assembles Xenia's PPC instruction tests (fetched into tools/xenia-tests by
# scripts/setup_tools.sh) plus our own (tests/instr/*.s, same format) with
# LLVM, recompiles them with XenonRecomp's test mode, builds XenonTests
# natively, and runs it. Any line the test binary prints is a failed register
# check. Exits non-zero when a test doesn't assemble, XenonTests doesn't
# build, or a check fails. Xenia's tests are required (they cover most of
# the instructions our patches add); SKIP_XENIA=1 runs only our own.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
die() { echo "run_instr_tests.sh: $*" >&2; exit 1; }
# LLVM's bin folder: LLVM=, else Homebrew's, else the clang on PATH's (checked
# here: without one, the folder would be "." and the error below would only
# name ./clang).
if [ -z "${LLVM:-}" ]; then
  if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then LLVM=/opt/homebrew/opt/llvm/bin
  elif command -v clang > /dev/null; then LLVM="$(dirname "$(command -v clang)")"
  else die "no clang on PATH: needs LLVM 18 or newer (clang, llvm-mc, llvm-objdump; on a Mac, Homebrew's llvm), or LLVM=<its bin folder>"; fi
fi
clang_line="$("$LLVM/clang" --version 2>/dev/null | head -1 || true)"
clang_major="$(echo "$clang_line" | sed -n 's/.*clang version \([0-9][0-9]*\).*/\1/p')"
case "$clang_line" in "Apple clang"*) clang_min=16 ;; *) clang_min=18 ;; esac
[ -n "$clang_major" ] && [ "$clang_major" -ge "$clang_min" ] ||
  die "$LLVM/clang is \"${clang_line:-missing}\": needs Clang 18 or newer (LLVM=<its bin folder>)"
# The linker: LLD=, else LLVM's own, else the one on PATH (Homebrew's lld
# formula puts it in /opt/homebrew/bin; Linux distributions in /usr/bin).
if [ -z "${LLD:-}" ]; then
  if [ -x "$LLVM/ld.lld" ]; then LLD="$LLVM/ld.lld"
  elif command -v ld.lld > /dev/null; then LLD="$(command -v ld.lld)"
  else LLD=/opt/homebrew/bin/ld.lld; fi
fi
for t in "$LLVM/clang" "$LLVM/llvm-mc" "$LLVM/llvm-objdump" "$LLD"; do
  [ -n "$t" ] && [ -x "$t" ] || { echo "missing ${t:-ld.lld}: needs LLVM's clang, llvm-mc and llvm-objdump (LLVM=<bin dir>) and lld (LLD=<ld.lld>)"; exit 1; }
done
XR="$ROOT/tools/XenonRecomp"
WORK="$ROOT/tools/instr-tests"
XENIA="$ROOT/tools/xenia-tests"
rm -rf "$WORK" && mkdir -p "$WORK/bin"

# Always test the current source: rebuild just the tools (not XenonTests, whose
# generated sources come and go), and stop if that fails.
ninja -C "$XR/build" XenonRecomp XenonAnalyse > "$WORK.build.log" 2>&1 || { cat "$WORK.build.log"; echo "XenonRecomp build failed"; exit 1; }

# Xenia's tests use gas syntax; one directory holds both sources, bin/ the
# objects. Xenia's are the ones setup_tools.sh fetched and listed with their
# SHA-256s (tools/xenia-tests/SHA256SUMS), all of them and unchanged: a
# missing or partial set would quietly test less.
if [ "${SKIP_XENIA:-0}" = 1 ]; then
  echo "SKIP_XENIA=1: only our own tests (tests/instr)"
else
  [ -s "$XENIA/SHA256SUMS" ] ||
    die "no Xenia instruction tests in tools/xenia-tests: run scripts/setup_tools.sh (or SKIP_XENIA=1 for only our own)"
  if command -v sha256sum > /dev/null; then check=(sha256sum --quiet -c SHA256SUMS); else check=(shasum -a 256 --quiet -c SHA256SUMS); fi
  (cd "$XENIA" && "${check[@]}") || die "tools/xenia-tests doesn't match its SHA256SUMS: run scripts/setup_tools.sh again"
  while read -r _ name; do cp "$XENIA/$name" "$WORK/"; done < "$XENIA/SHA256SUMS"
fi
cp "$ROOT"/tests/instr/*.s "$WORK"/
for s in "$WORK"/*.s; do
  [ -s "$s" ] || die "EMPTY TEST FILE: $(basename "$s") (rerun scripts/setup_tools.sh)"
done
if [ $# -gt 0 ]; then
  for f in "$WORK"/*.s; do
    keep=0; for t in "$@"; do [ "$(basename "$f" .s)" = "$t" ] && keep=1; done
    [ $keep = 1 ] || rm "$f"
  done
  compgen -G "$WORK/*.s" > /dev/null || die "no test named $*"
fi

fail_asm=0
for s in "$WORK"/*.s; do
  n=$(basename "$s" .s)
  # LLVM wants %-prefixed register names; Xenia's tests use bare ones.
  perl -pe 'next if /^\s*#/; s/\b(r|v|f|cr)(\d+)\b/%$1$2/g unless /:\s*$/' "$s" > "$WORK/bin/$n.llvm.s"
  # XenonUtils' ELF loader wants a *linked* 32-bit big-endian image (like
  # Xenia's own test build), so assemble for ppc32 and link low at 0x10000 (the harness masks addresses to 20 bits).
  if ! "$LLVM"/llvm-mc -triple=powerpc-unknown-unknown -mcpu=pwr7 -mattr=+altivec \
       -filetype=obj -o "$WORK/bin/$n.obj" "$WORK/bin/$n.llvm.s" 2> "$WORK/bin/$n.asm.log"; then
    echo "ASSEMBLE FAILED: $n ($(head -1 "$WORK/bin/$n.asm.log"))"; rm -f "$s"; fail_asm=$((fail_asm + 1)); continue
  fi
  # The harness only picks up *.o, so the linked executable takes that name.
  "$LLD" -o "$WORK/bin/$n.o" -e 0 --image-base=0 -Ttext=0x10000 "$WORK/bin/$n.obj"
  "$LLVM"/llvm-objdump -d "$WORK/bin/$n.o" > "$WORK/bin/$n.dis"
done

# Recompile into the XenonTests project directory (it globs *.cpp).
rm -f "$XR"/XenonTests/*.cpp
if ! "$XR"/build/XenonRecomp/XenonRecomp "$WORK/bin" "$XR/XenonTests" > "$WORK/recompile.log" 2>&1; then
  grep -v "^Recompiling" "$WORK/recompile.log" || true
  die "XenonRecomp's test mode failed"
fi
grep -v "^Recompiling" "$WORK/recompile.log" || true

cmake -Wno-deprecated -S "$XR" -B "$XR/build-tests" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$LLVM/clang" -DCMAKE_CXX_COMPILER="$LLVM/clang++" \
  "-DCMAKE_CXX_FLAGS=-include cstdlib" > /dev/null
# The binary of an earlier run goes first: if this build fails, nothing stale
# is left to run and pass.
TESTS_BIN="$XR/build-tests/XenonTests/XenonTests"
rm -f "$TESTS_BIN"
if ! ninja -C "$XR/build-tests" XenonTests > "$WORK/tests.build.log" 2>&1; then
  grep -E "error|FAILED" "$WORK/tests.build.log" | head -20
  die "XenonTests didn't build (the whole log: $WORK/tests.build.log)"
fi
echo "---- running ----"
# XenonTests prints one line per failed check and returns 0; a crash ends
# the run too (every test after it unrun).
status=0
"$TESTS_BIN" > "$WORK/results.txt" || status=$?
cat "$WORK/results.txt"
failed=$(wc -l < "$WORK/results.txt" | tr -d ' ')
summary="$failed failed checks"
[ "$fail_asm" = 0 ] || summary="$summary, $fail_asm test(s) that didn't assemble"
[ "$status" = 0 ] || summary="$summary, XenonTests exited with status $status"
echo "---- $summary ----"
[ "$failed" = 0 ] && [ "$fail_asm" = 0 ] && [ "$status" = 0 ]
