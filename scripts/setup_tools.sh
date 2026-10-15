#!/usr/bin/env bash
# Rebuild the toolchain from scratch into tools/ (git-ignored).
#
#   scripts/setup_tools.sh
#
# - XenonRecomp at a pinned upstream commit + patches/xenonrecomp/*.patch
#   (branch "nfsmw"), built with Clang.
# - extract-xiso (to unpack the disc image), at a pinned commit.
# - Xenia's PPC instruction tests for the instructions we added, at a pinned
#   commit, each checked against its SHA-256 (SKIP_XENIA=1 skips them: only
#   tests/run_instr_tests.sh needs them).
#
# Each tool is remade when what it is made from changes, not only when it is
# missing: tools/.stamps/<tool> records its inputs (the pinned commit, the
# patches' SHA-256s, the FFmpeg version and build script), so a pull that
# adds or changes a patch, or moves a pin, takes effect at the next run.
# scripts/recompile.sh reads XenonRecomp's record to refuse an old build.
#
# Needs: git, cmake, ninja, clang 18+ (Homebrew LLVM on macOS, the system
# clang elsewhere; override with LLVM=<bin dir>), lld, make, curl.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$ROOT/tools"
STAMPS="$TOOLS/.stamps"
die() { echo "setup_tools.sh: $*" >&2; exit 1; }

# The compiler, checked first: without a clang on PATH the bin folder would
# be "." (dirname of nothing), and the failure would come much later, from
# CMake, about a compiler named "./clang".
if [ -z "${LLVM:-}" ]; then
  if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then LLVM=/opt/homebrew/opt/llvm/bin
  elif command -v clang > /dev/null; then LLVM="$(dirname "$(command -v clang)")"
  else die "no clang on PATH: install Clang 18 or newer (on a Mac, Homebrew's llvm), or set LLVM=<its bin folder>"; fi
fi
[ -x "$LLVM/clang" ] && [ -x "$LLVM/clang++" ] || die "no clang and clang++ in $LLVM (LLVM=<the bin folder of Clang 18 or newer>)"
# "clang version 18.1.3", "Ubuntu clang version ...", "Homebrew clang version
# ...", or Apple's own numbering ("Apple clang version 17.0.0", Xcode 26), as
# CMakeLists.txt accepts them.
clang_line="$("$LLVM/clang" --version 2>/dev/null | head -1)"
clang_major="$(echo "$clang_line" | sed -n 's/.*clang version \([0-9][0-9]*\).*/\1/p')"
case "$clang_line" in "Apple clang"*) clang_min=16 ;; *) clang_min=18 ;; esac
[ -n "$clang_major" ] && [ "$clang_major" -ge "$clang_min" ] ||
  die "$LLVM/clang is \"${clang_line:-not a clang}\": needs Clang 18 or newer (LLVM=<its bin folder>)"

sha256() { if command -v sha256sum >/dev/null; then sha256sum -c - > /dev/null; else shasum -a 256 -c - > /dev/null; fi; }
sha256_of() { if command -v sha256sum > /dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
stamp() { cat "$STAMPS/$1" 2>/dev/null || true; }
XENONRECOMP_BASE=ddd128bcca99fe8bfbb99bea583c972351fa6ace   # hedge-dev/XenonRecomp main, 2025
EXTRACT_XISO_COMMIT=3f5b62cfe68f000b0e3c8a30104973f3a297948e  # XboxDev/extract-xiso master, 2026-09-11

mkdir -p "$TOOLS" "$STAMPS"

# XenonRecomp's inputs: the base commit and each patch, one line each (the
# lines scripts/recompile.sh compares, so keep their form).
xenonrecomp_inputs="$(echo "base $XENONRECOMP_BASE"
  for p in "$ROOT"/patches/xenonrecomp/*.patch; do echo "patches/xenonrecomp/$(basename "$p") $(sha256_of "$p")"; done)"
if [ ! -d "$TOOLS/XenonRecomp" ]; then
  git clone --recursive https://github.com/hedge-dev/XenonRecomp.git "$TOOLS/XenonRecomp"
fi
cd "$TOOLS/XenonRecomp"
if ! git rev-parse -q --verify nfsmw >/dev/null || [ "$(stamp xenonrecomp)" != "$xenonrecomp_inputs" ]; then
  # The branch is remade from the base with every patch: one made from other
  # patches (an earlier release's, or before this record existed) would keep
  # building the old recompiler. Never over uncommitted work; the old branch
  # stays reachable through git reflog.
  git am --abort > /dev/null 2>&1 || true   # a run that stopped in the middle of git am
  [ -z "$(git status --porcelain --untracked-files=no --ignore-submodules=all)" ] ||
    die "tools/XenonRecomp has uncommitted changes, and its branch nfsmw must be remade from patches/xenonrecomp: commit or discard them first"
  old="$(git rev-parse -q --verify nfsmw || true)"
  rm -f "$STAMPS/xenonrecomp"
  git cat-file -e "$XENONRECOMP_BASE^{commit}" 2>/dev/null || git fetch -q origin
  git checkout -q -B nfsmw "$XENONRECOMP_BASE"
  git submodule update --init --recursive
  # The patches carry their own authors; the committer needs any name (a fresh
  # machine or a CI runner has none configured).
  git -c user.name="SpeedBreaker setup" -c user.email=setup@speedbreaker.invalid \
    am -q "$ROOT"/patches/xenonrecomp/*.patch
  [ -z "$old" ] || echo "tools/XenonRecomp: branch nfsmw remade from patches/xenonrecomp (it was $old)"
fi
# The bundled fmt predates libc++ dropping transitive <cstdlib>.
cmake -Wno-deprecated -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$LLVM/clang" -DCMAKE_CXX_COMPILER="$LLVM/clang++" \
  "-DCMAKE_CXX_FLAGS=-include cstdlib" > /dev/null
ninja -C build XenonRecomp XenonAnalyse
echo "$xenonrecomp_inputs" > "$STAMPS/xenonrecomp"

# extract-xiso at its pinned commit (an existing checkout, shallow or not, is
# moved to it).
if [ "$(stamp extract-xiso)" != "$EXTRACT_XISO_COMMIT" ] || [ ! -x "$TOOLS/extract-xiso/build/extract-xiso" ]; then
  rm -f "$STAMPS/extract-xiso"
  [ -d "$TOOLS/extract-xiso/.git" ] || git clone -q https://github.com/XboxDev/extract-xiso.git "$TOOLS/extract-xiso"
  git -C "$TOOLS/extract-xiso" cat-file -e "$EXTRACT_XISO_COMMIT^{commit}" 2>/dev/null ||
    git -C "$TOOLS/extract-xiso" fetch -q origin "$EXTRACT_XISO_COMMIT"
  git -C "$TOOLS/extract-xiso" -c advice.detachedHead=false checkout -q "$EXTRACT_XISO_COMMIT"
  cmake -Wno-deprecated -S "$TOOLS/extract-xiso" -B "$TOOLS/extract-xiso/build" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
  ninja -C "$TOOLS/extract-xiso/build"
  echo "$EXTRACT_XISO_COMMIT" > "$STAMPS/extract-xiso"
fi

# FFmpeg for XMA audio: a minimal static libavcodec (XMA1/XMA2/WMAPro
# decoders only) with patches/ffmpeg applied (scripts/build_ffmpeg_xma.sh).
# On macOS it is linked into the game, so it is built for the oldest macOS
# the game runs on (CMakeLists.txt's SB_OSX_DEFAULT_TARGET), not this Mac's
# own; one built for another macOS is rebuilt. Rebuilt too when the version,
# patches/ffmpeg or the build script change (its record, tools/.stamps/ffmpeg-xma).
FFMPEG_VERSION=7.1.1
FFMPEG_SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
FFMPEG_TARBALL="ffmpeg-$FFMPEG_VERSION.tar.xz"
ffmpeg_ok=1
[ -f "$TOOLS/ffmpeg-xma/lib/libavcodec.a" ] || ffmpeg_ok=0
if [ "$(uname -s)" = Darwin ]; then
  MACOSX_DEPLOYMENT_TARGET="$(sed -n 's/^set(SB_OSX_DEFAULT_TARGET "\([0-9.]*\)").*/\1/p' "$ROOT/CMakeLists.txt" | head -1)"
  export MACOSX_DEPLOYMENT_TARGET
  [ -n "$MACOSX_DEPLOYMENT_TARGET" ] || { echo "no SB_OSX_DEFAULT_TARGET in $ROOT/CMakeLists.txt"; exit 1; }
  if [ $ffmpeg_ok = 1 ] && [ "$(otool -l "$TOOLS/ffmpeg-xma/lib/libavcodec.a" "$TOOLS/ffmpeg-xma/lib/libavutil.a" \
       | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; b=0}' | sort -u)" != "$MACOSX_DEPLOYMENT_TARGET" ]; then
    echo "tools/ffmpeg-xma isn't built for macOS $MACOSX_DEPLOYMENT_TARGET: rebuilding it"; ffmpeg_ok=0
  fi
fi
ffmpeg_inputs="$(echo "ffmpeg $FFMPEG_VERSION $FFMPEG_SHA256"
  echo "scripts/build_ffmpeg_xma.sh $(sha256_of "$ROOT/scripts/build_ffmpeg_xma.sh")"
  for p in "$ROOT"/patches/ffmpeg/*.patch; do echo "patches/ffmpeg/$(basename "$p") $(sha256_of "$p")"; done
  [ -z "${MACOSX_DEPLOYMENT_TARGET:-}" ] || echo "macOS $MACOSX_DEPLOYMENT_TARGET")"
if [ $ffmpeg_ok = 1 ] && [ "$(stamp ffmpeg-xma)" != "$ffmpeg_inputs" ]; then
  echo "tools/ffmpeg-xma was built from another FFmpeg version, patches/ffmpeg or build script: rebuilding it"; ffmpeg_ok=0
fi
if [ $ffmpeg_ok = 0 ]; then
  rm -f "$STAMPS/ffmpeg-xma"
  mkdir -p "$TOOLS/ffmpeg-src"
  cd "$TOOLS/ffmpeg-src"
  # Downloaded under a temporary name and checked before it takes the real
  # one: an interrupted download left under the real name would fail every
  # later run's check, with nothing saying why. A damaged archive already
  # there (from before) is fetched again.
  if [ -f "$FFMPEG_TARBALL" ] && ! echo "$FFMPEG_SHA256  $FFMPEG_TARBALL" | sha256 2>/dev/null; then
    echo "tools/ffmpeg-src/$FFMPEG_TARBALL is damaged or incomplete: downloading it again"; rm -f "$FFMPEG_TARBALL"
  fi
  if [ ! -f "$FFMPEG_TARBALL" ]; then
    curl -sfL -o "$FFMPEG_TARBALL.part" "https://ffmpeg.org/releases/$FFMPEG_TARBALL" ||
      { rm -f "$FFMPEG_TARBALL.part"; die "couldn't download https://ffmpeg.org/releases/$FFMPEG_TARBALL"; }
    echo "$FFMPEG_SHA256  $FFMPEG_TARBALL.part" | sha256 ||
      { rm -f "$FFMPEG_TARBALL.part"; die "https://ffmpeg.org/releases/$FFMPEG_TARBALL doesn't have the SHA-256 $FFMPEG_SHA256"; }
    mv -f "$FFMPEG_TARBALL.part" "$FFMPEG_TARBALL"
  fi
  "$ROOT/scripts/build_ffmpeg_xma.sh" "$TOOLS/ffmpeg-src/$FFMPEG_TARBALL" "$TOOLS/ffmpeg-xma"
  # The license texts, for the SteamOS bundle's licenses/ (make_bundle.sh).
  tar -xf "$FFMPEG_TARBALL" "ffmpeg-$FFMPEG_VERSION/COPYING.LGPLv2.1" "ffmpeg-$FFMPEG_VERSION/LICENSE.md"
  echo "$ffmpeg_inputs" > "$STAMPS/ffmpeg-xma"
  cd "$ROOT"
fi

# Xenia's instruction tests, from raw.githubusercontent.com at a pinned
# commit (no GitHub login), each checked against its SHA-256 here. Into a
# temporary file first: a failed or wrong fetch must not leave a test that
# later runs would take as fetched. tools/xenia-tests/SHA256SUMS lists them
# for tests/run_instr_tests.sh, which runs exactly these.
XENIA_COMMIT=95a5c3ee250f80c3b9d139658649d9ffb6db3eec   # xenia-project/xenia master, 2026-10-15
XENIA_TESTS="
0af0897192c2e9c01c6d766de0c19aab368f71ce4c63c284ba520e6866233c20  instr_mulhd.s
cb1afa738e06b8e418a4c107cb562430b8675706fa71b82657a66fc7adc11a63  instr_mulhdu.s
0e382b4c56f91d6ef78420203873de3f2b042fa25b31947a2245860bab9eaed2  instr_mulhw.s
817f5b114084118090b8aeddf61f00f444407c5f25f91aaa8db3e9d9ff0131f9  instr_vand.s
e3064638d6b7fd9bc08c874a2e6f74deb7b75dfa2311beb00ed149babb5d6288  instr_vandc.s
5d35bc1a531ee8915e087eb1c68a1e019b88c68cd34dc1bcfc7d255521bdf44d  instr_vavguh.s
baf1307cbef36a57efa3798023bf41b5d36ce8b0119b92e8d3a11c90e022e2f8  instr_vctuxs.s
7a223c6eecfc642b67c9673f5b03dbff7ea40458deb58ae26c367434c8d592b1  instr_vmaxsh.s
10b6c12ac34e9f4f453c6379c7dca645899acee83478264ec023504f4f28f778  instr_vminsh.s
0a8185b9820ab88abe824cb45319c7dd881426b6a299307f194fac17c6659bc8  instr_vpkshss.s
cab70a3ad769098dbd53b34a8e64b767aceafb7d90ffa1aa65c7b9eff64a1fb7  instr_vpkswss.s
13f5aedbbd047298fbf9db489c006fb9f90c39af771ccd7316e266739bcb2249  instr_vpkswus.s
b9e9d4042fdbc777513a1026549d9b4e2dbccf4ce6745ede24c3e432bec68f82  instr_vpkuhus.s
b953db3314989e946a818284b268cd3437cb316aa033a07f4ad081f68279ffa1  instr_vrlh.s
00202e0c9c0d048ebbd46bb6ffea56968ff5b006e12fee577182b147bac9f008  instr_vsel.s
561942e5c77bdd78ddb1b81007fa0c468a20da15f433407080165de9832ad5ab  instr_vslh.s
0210ba43bb9b5cdb4afc59e38864626246f78a138490b70b5f527c0ea8f7ce29  instr_vslw.s
91ced4aed2ced0e9ce2c3dfdfcf585b811ac4447797ee15134783b81e591982e  instr_vspltish.s
4a57004a2082c8b674a7461f4d3bda983b494d50a22475aa0a00619e11a62bbe  instr_vsrah.s
4e1de55d5569522e1a0a9fd7d18dd9e8ec7e79e550c287a7c4ef6d0830d78c0f  instr_vsrh.s
79b28f8aaa7291fab4635c0f663dcb010918ac45d9c8c71e38ba03865b66d599  instr_vsubshs.s
"
if [ "${SKIP_XENIA:-0}" = 1 ]; then
  echo "SKIP_XENIA=1: not fetching Xenia's instruction tests (tests/run_instr_tests.sh needs them, or SKIP_XENIA=1 there too)"
else
  mkdir -p "$TOOLS/xenia-tests"
  rm -f "$TOOLS/xenia-tests/SHA256SUMS"
  while read -r hash name; do
    [ -n "$name" ] || continue
    f="$TOOLS/xenia-tests/$name"
    [ -f "$f" ] && [ "$(sha256_of "$f")" = "$hash" ] && continue
    url="https://raw.githubusercontent.com/xenia-project/xenia/$XENIA_COMMIT/src/xenia/cpu/ppc/testing/$name"
    if curl -sfL -o "$f.part" "$url" && [ "$(sha256_of "$f.part")" = "$hash" ]; then
      mv -f "$f.part" "$f"
    else
      rm -f "$f.part"
      die "couldn't fetch Xenia's $name with the SHA-256 $hash ($url); SKIP_XENIA=1 skips Xenia's tests (only tests/run_instr_tests.sh needs them)"
    fi
  done <<< "$XENIA_TESTS"
  echo "$XENIA_TESTS" | sed '/^$/d' > "$TOOLS/xenia-tests/SHA256SUMS"
fi

echo "tools ready: $(git -C "$TOOLS/XenonRecomp" log --oneline -1)"
