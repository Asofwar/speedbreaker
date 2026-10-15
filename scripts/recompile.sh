#!/usr/bin/env bash
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
#   scripts/recompile.sh
#
# Translates the game's code into C++ (ppc/): XenonRecomp (tools/, built by
# scripts/setup_tools.sh) on your own executable, game/files/default.xex,
# with config/nfsmw.toml. The same as README.md's manual command, plus:
#   - the executable must be the supported one (the SHA-256 in
#     config/nfsmw.toml's header), checked before anything is written;
#   - XenonRecomp must be built from the current patches/xenonrecomp (the
#     record setup_tools.sh keeps in tools/.stamps/xenonrecomp): after a pull
#     that changed them, setup_tools.sh comes first;
#   - files an earlier run made that this one doesn't are removed. XenonRecomp
#     leaves a file it would write identically untouched (so ninja rebuilds
#     only what changed), and never deletes one: with fewer functions, an old
#     ppc_recomp.<n>.cpp past the new last one would stay, and be compiled in
#     (recomp/CMakeLists.txt takes every ppc/*.cpp). It runs into a fresh
#     folder, and only the files that differ are copied over;
#   - ppc/.inputs records what ppc/ was made from: the executable's SHA-256,
#     each config/*.toml's and patches/xenonrecomp/*.patch's, and
#     XenonRecomp's commit. CMake compares it with the checkout when it
#     configures (recomp/CMakeLists.txt; ninja configures again when one of
#     them changes), so a pull that changes the config or the patches stops
#     the build until this runs again, instead of quietly building the old
#     ppc/ into the new runtime.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XR="$ROOT/tools/XenonRecomp"
RECOMP="$XR/build/XenonRecomp/XenonRecomp"
XEX="$ROOT/game/files/default.xex"
PPC="$ROOT/ppc"
die() { echo "recompile.sh: $*" >&2; exit 1; }
sha256_of() { if command -v sha256sum > /dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }

[ -x "$RECOMP" ] && [ -f "$XR/XenonUtils/ppc_context.h" ] || die "no XenonRecomp in tools/: run scripts/setup_tools.sh first"

# The tool against the patches: setup_tools.sh's record (the base commit and
# each patch's SHA-256, in the same lines as below) must match this checkout.
base="$(sed -n 's/^XENONRECOMP_BASE=\([0-9a-f]\{40\}\).*/\1/p' "$ROOT/scripts/setup_tools.sh")"
[ -n "$base" ] || die "no XENONRECOMP_BASE in scripts/setup_tools.sh"
patches() { local p; for p in "$ROOT"/patches/xenonrecomp/*.patch; do echo "patches/xenonrecomp/$(basename "$p") $(sha256_of "$p")"; done; }
want_tool="$(echo "base $base"; patches)"
if [ "$(cat "$ROOT/tools/.stamps/xenonrecomp" 2>/dev/null)" != "$want_tool" ]; then
    die "tools/XenonRecomp isn't built from this checkout's patches/xenonrecomp (or was set up before scripts/setup_tools.sh recorded them): run scripts/setup_tools.sh first"
fi

# The executable: the supported one only (README.md, "Building from source").
want_xex="$(sed -n 's/^# default\.xex SHA-256 \([0-9a-f]\{64\}\).*/\1/p' "$ROOT/config/nfsmw.toml" | head -1)"
[ -n "$want_xex" ] || die "no \"# default.xex SHA-256 <hash>\" line in config/nfsmw.toml"
[ -f "$XEX" ] || die "no game/files/default.xex: extract your own disc image into game/files first (README.md, step 2)"
have_xex="$(sha256_of "$XEX")"
[ "$have_xex" = "$want_xex" ] || die "game/files/default.xex has SHA-256 $have_xex, not the supported $want_xex (the USA disc's; README.md)"

# A copy of config/ beside a fresh output folder: the config's paths are
# relative to it (../game/files/default.xex, ../ppc), and XenonRecomp joins
# them to its folder, so an absolute path can't stand in for them.
WORK="$(mktemp -d "${TMPDIR:-/tmp}/sb-recompile.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/config" "$WORK/game/files" "$WORK/ppc"
cp "$ROOT"/config/*.toml "$WORK/config/"
ln -s "$XEX" "$WORK/game/files/default.xex"
"$RECOMP" "$WORK/config/nfsmw.toml" "$XR/XenonUtils/ppc_context.h"
[ -s "$WORK/ppc/ppc_config.h" ] && [ -s "$WORK/ppc/ppc_recomp.0.cpp" ] || die "XenonRecomp made no ppc_config.h or ppc_recomp.0.cpp"

# Into ppc/ (itself kept: it may be a link or a mount): the record goes
# first, so an interrupted copy leaves no record that vouches for it; then
# the changed files, then the old ones this run didn't make.
mkdir -p "$PPC"
rm -f "$PPC/.inputs"
changed=0
for f in "$WORK"/ppc/*; do
    name="$(basename "$f")"
    if ! cmp -s "$f" "$PPC/$name"; then
        cp "$f" "$PPC/$name.part" && mv -f "$PPC/$name.part" "$PPC/$name"
        changed=$((changed + 1))
    fi
done
removed=0
for f in "$PPC"/*.cpp "$PPC"/*.h; do
    [ -f "$f" ] || continue
    if [ ! -e "$WORK/ppc/$(basename "$f")" ]; then
        rm -f "$f"
        removed=$((removed + 1))
    fi
done

{
    echo "# What ppc/ was made from, written by scripts/recompile.sh; recomp/CMakeLists.txt"
    echo "# compares it with the checkout. One \"<input> <SHA-256 or commit>\" per line."
    echo "default.xex $have_xex"
    for f in "$ROOT"/config/*.toml; do echo "config/$(basename "$f") $(sha256_of "$f")"; done
    patches
    echo "xenonrecomp $(git -C "$XR" rev-parse HEAD 2>/dev/null || echo unknown)"
} > "$PPC/.inputs.part"
mv -f "$PPC/.inputs.part" "$PPC/.inputs"
echo "ppc/: $(find "$PPC" -maxdepth 1 -name '*.cpp' | wc -l | tr -d ' ') sources; $changed file(s) written, $removed old one(s) removed; recorded in ppc/.inputs"
