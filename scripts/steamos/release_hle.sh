#!/bin/bash
# release_hle.sh <tag> [build dir] [--publish]: the SteamOS x86-64 release of
# the HLE fork (Asofwar/speedbreaker-hle), from a Linux build on Arch
# (scripts/steamos/make_bundle.sh's requirements; the sb-build container).
#
# The bundle is make_bundle.sh's, with its GNU C Library and GCC runtime
# (lib/glibc, lib/cxx) and their license files taken from the original
# project's release of the same version: their complete source is then the
# original release's SpeedBreaker-<version>-third-party-source.tar.xz, which
# is attached to the fork's release too (LGPL-2.1 / GPL-3.0 corresponding
# source, as THIRD_PARTY_NOTICES.md describes; FFmpeg is built by the same
# scripts/build_ffmpeg_xma.sh from the same version). The binary needs
# GLIBC_2.44 and GLIBCXX_3.4.36 at most (make_bundle.sh's line says so); the
# check below refuses a build that needs more than those libraries give.
#
# Without --publish it only makes the files, in dist/<tag>/. With it, it also
# creates the GitHub release <tag> (gh, logged in) with them and its notes
# (dist/<tag>/notes.md: the fork's section of README.md and the commits since
# the previous release tag).
set -euo pipefail
TAG="${1:?usage: release_hle.sh <tag> [build dir] [--publish]}"
BUILD="${2:-build}"
PUBLISH=0
for a in "$@"; do [ "$a" = "--publish" ] && PUBLISH=1; done
REPO="Asofwar/speedbreaker-hle"
UP="SpeedBreakerProject/speedbreaker"
cd "$(dirname "$0")/../.."
ROOT="$PWD"
VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$BUILD/CMakeCache.txt")"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "release_hle.sh: no version in $BUILD/CMakeCache.txt" >&2; exit 1; }
OUT="$ROOT/dist/$TAG"
rm -rf "$OUT"
mkdir -p "$OUT/up"

# 1. The bundle, and the original release's of the same version.
scripts/steamos/make_bundle.sh "$BUILD" "$OUT/bundle"
gh release download "v$VERSION" -R "$UP" -D "$OUT/up" \
    -p SpeedBreaker-steamos-x86_64.tar.xz -p "SpeedBreaker-$VERSION-third-party-source.tar.xz" \
    -p "SpeedBreaker-$VERSION-third-party-source.tar.xz.sha256"
(cd "$OUT/up" && sha256sum -c "SpeedBreaker-$VERSION-third-party-source.tar.xz.sha256")
tar -C "$OUT/up" -xJf "$OUT/up/SpeedBreaker-steamos-x86_64.tar.xz"
STAGE="$OUT/bundle/SpeedBreaker"
UPB="$OUT/up/SpeedBreaker"

# 2. The original release's C library and C++ runtime, and their licenses.
need_glibc="$(objdump -T "$STAGE/lib/SpeedBreaker" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
need_cxx="$(objdump -T "$STAGE/lib/SpeedBreaker" | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -1)"
objdump -T "$UPB/lib/glibc/libc.so.6" | grep -q " $need_glibc " || { echo "the original release's glibc has no $need_glibc" >&2; exit 1; }
objdump -T "$UPB/lib/cxx/libstdc++.so.6" | grep -q " $need_cxx " || { echo "the original release's libstdc++ has no $need_cxx" >&2; exit 1; }
for d in lib/glibc lib/cxx licenses/glibc licenses/libgcc licenses/libstdc++; do
    rm -rf "${STAGE:?}/$d"
    cp -a "$UPB/$d" "$STAGE/$d"
done
python3 - "$STAGE/licenses/README.txt" "$UPB/licenses/README.txt" <<'PY'
import sys
ours, theirs = sys.argv[1], sys.argv[2]
mine = open(ours, encoding="utf-8").read().splitlines()
up = [l for l in open(theirs, encoding="utf-8").read().splitlines() if l.startswith(("  glibc/", "  cxx/"))]
out, done = [], False
for l in mine:
    if l.startswith(("  glibc/", "  cxx/")):
        if not done:
            out += up
            done = True
        continue
    out.append(l)
open(ours, "w", encoding="utf-8").write("\n".join(out) + "\n")
PY
echo "HLE fork release $TAG (https://github.com/$REPO): the C library and C++ runtime are v$VERSION's of the original project" >> "$STAGE/bundle-info.txt"
(cd "$STAGE" && find . -type f ! -name SHA256SUMS -print0 | sort -z | sed -z 's|^\./||' | xargs -0 sha256sum > SHA256SUMS)

# 3. The downloads.
tar -C "$OUT/bundle" --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$(stat -c %Y "$STAGE/lib/SpeedBreaker")" -cf - SpeedBreaker |
    xz -T0 -9 > "$OUT/SpeedBreaker-steamos-x86_64.tar.xz"
cp "$OUT/up/SpeedBreaker-$VERSION-third-party-source.tar.xz" "$OUT/"
(cd "$OUT" && for f in SpeedBreaker-steamos-x86_64.tar.xz "SpeedBreaker-$VERSION-third-party-source.tar.xz"; do sha256sum "$f" > "$f.sha256"; done)

# 4. The notes.
prev="$(git describe --tags --abbrev=0 --match 'v*-hle*' HEAD^ 2>/dev/null || echo "v$VERSION")"
{
    echo "SpeedBreaker v$VERSION with the HLE renderer work for the Steam Deck at 5 W (fork of $UP)."
    echo
    echo "Install: unpack \`SpeedBreaker-steamos-x86_64.tar.xz\` (for example into \`~/Games\`) and add \`SpeedBreaker/speedbreaker.sh\` to Steam as a non-Steam game; the installer asks for a backup of your own USA Xbox 360 disc. No game files are included."
    echo
    echo "Установка: распакуйте \`SpeedBreaker-steamos-x86_64.tar.xz\` (например в \`~/Games\`) и добавьте \`SpeedBreaker/speedbreaker.sh\` в Steam как стороннюю игру; установщик попросит образ вашего собственного диска Xbox 360 (США). Файлы игры в сборку не входят."
    echo
    echo "Changes since $prev:"
    git log --format='- %s' "$prev..HEAD" --no-merges | head -80
    echo
    echo "\`SpeedBreaker-$VERSION-third-party-source.tar.xz\`: the complete source of the bundled GNU C Library, GCC runtime and FFmpeg (the original release's: the same libraries)."
} > "$OUT/notes.md"

ls -la "$OUT"
if [ "$PUBLISH" = 1 ]; then
    gh release create "$TAG" -R "$REPO" --title "SpeedBreaker HLE $TAG" --notes-file "$OUT/notes.md" \
        "$OUT/SpeedBreaker-steamos-x86_64.tar.xz" "$OUT/SpeedBreaker-steamos-x86_64.tar.xz.sha256" \
        "$OUT/SpeedBreaker-$VERSION-third-party-source.tar.xz" "$OUT/SpeedBreaker-$VERSION-third-party-source.tar.xz.sha256"
fi
