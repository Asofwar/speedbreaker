#!/bin/bash
# The SteamOS launcher's choices before the game starts (scripts/steamos/
# bundle/speedbreaker.sh): the x86-64-v3 check (cpu_v3_missing), the
# system's libstdc++ (host_cxx_lib), the C and C++ runtimes the game runs
# with (loader_version, cxx_version, use_bundled), the library folder the
# loader is given (open_lib_dir) and the log's last place (private_log_dir),
# each run alone on made-up loaders (scripts that answer --help and
# --version as ld.so does), libraries (a 64- or 32-bit ELF header and the
# GLIBCXX_ names cxx_version reads) and folders. Then the whole launcher on
# a made-up bundle in a folder named with ':', ';' and '$', which the
# loader's --library-path can't take as it is: its program (a copy of xz)
# must start, with its library (liblzma) from the bundle's lib/ and from
# lib/cxx. Linux only (registered by tests/CMakeLists.txt), or:
#   bash tests/launcher_start_test.sh scripts/steamos/bundle/speedbreaker.sh
set -u
LAUNCHER=${1:?usage: launcher_start_test.sh <speedbreaker.sh>}
[ -f "$LAUNCHER" ] || { echo "no $LAUNCHER"; exit 1; }
LAUNCHER="$(cd "$(dirname "$LAUNCHER")" && pwd)/$(basename "$LAUNCHER")"
# The functions, as the launcher has them: each from "name() {" to its "}",
# or a one-line one.
NAMES='cpu_v3_missing|loader_version|cxx_version|older|elf64_for|host_cxx_lib|use_bundled|open_lib_dir|private_log_dir'
FUNCS="$(awk -v names="$NAMES" '
    $0 ~ "^(" names ")\\(\\) \\{" { if ($0 ~ /\}( +#.*)?$/) { print; next } on = 1 }
    on { print }
    on && /^\}$/ { on = 0 }' "$LAUNCHER")"
for name in ${NAMES//|/ }; do
    case "$FUNCS" in *"$name() {"*) ;; *) echo "FAIL: $name not found in $LAUNCHER"; exit 1 ;; esac
done
eval "$FUNCS"

T="$(mktemp -d "${TMPDIR:-/tmp}/launcher_start_test.XXXXXX")" || exit 1
trap 'chmod -R u+w "$T" 2>/dev/null; rm -rf "$T"' EXIT
failures=0
cases=0
check() {  # <shell test>; check <description>: counts that test's result
    local result=$? what=$1
    cases=$((cases + 1))
    if [ $result = 0 ]; then echo "  ok: $what"; else failures=$((failures + 1)); echo "  FAIL: $what"; fi
}
has() { case "$1" in *"$2"*) return 0 ;; *) return 1 ;; esac; }
# The machine host_cxx_lib asks about, and the ld.so.cache it reads.
FAKE_ARCH=x86_64
uname() { if [ "${1:-}" = -m ]; then echo "$FAKE_ARCH"; else command uname "$@"; fi; }
LDCONFIG=""
ldconfig() { [ -n "$LDCONFIG" ] && [ "${1:-}" = -p ] && printf '%s\n' "$LDCONFIG"; }

# loader <file> <glibc version or -> <hwcaps line for x86-64-v3 or ->: a
# made-up ld.so. "-" for no --version answer (it can't run), or for a --help
# without glibc-hwcaps (glibc before 2.33).
loader() {
    {
        echo '#!/bin/sh'
        echo 'case "$1" in'
        [ "$2" = - ] || echo "--version) echo 'ld.so (GNU libc) stable release version $2.'; echo 'Copyright (C) 2026 Free Software Foundation, Inc.' ;;"
        if [ "$3" = - ]; then
            echo "--help) echo 'Usage: ld.so [OPTION]... EXECUTABLE-FILE [ARGS-FOR-PROGRAM...]' ;;"
        else
            echo "--help) printf '%s\n' 'Subdirectories of glibc-hwcaps directories, in priority order:' '  x86-64-v4' '  $3' '  x86-64-v2 (supported, searched)' ;;"
        fi
        echo 'esac'
    } > "$1"
    chmod +x "$1"
}
# cpuinfo <file> <flags...>
cpuinfo() { printf 'processor\t: 0\nflags\t\t: %s\n' "$*" > "$1"; }
# The flags of a CPU with all of x86-64-v3 (a Steam Deck's Zen 2, trimmed), and
# of the CPUs just before it.
V3="fpu cx8 cmov sse sse2 pni ssse3 fma cx16 sse4_1 sse4_2 movbe popcnt xsave avx f16c lahf_lm abm bmi1 avx2 bmi2"
IVY_BRIDGE="fpu cx8 cmov sse sse2 pni ssse3 cx16 sse4_1 sse4_2 popcnt xsave avx f16c lahf_lm"
# Every flag the launcher checked before (avx2 fma bmi2 movbe), but no F16C
# and no LZCNT: what a VM's CPU model can look like. The game's code uses both.
MASKED="fpu cx8 cmov sse sse2 pni ssse3 fma cx16 sse4_1 sse4_2 movbe popcnt xsave avx lahf_lm bmi1 avx2 bmi2"

echo "x86-64-v3: the loader's answer first, else /proc/cpuinfo's flags"
mkdir -p "$T/cpu"
loader "$T/cpu/v3" 2.41 'x86-64-v3 (supported, searched)'
loader "$T/cpu/notv3" 2.41 'x86-64-v3'
loader "$T/cpu/old" 2.31 -
cpuinfo "$T/cpu/v3.txt" $V3; cpuinfo "$T/cpu/ivy.txt" $IVY_BRIDGE; cpuinfo "$T/cpu/masked.txt" $MASKED
out="$(cpu_v3_missing "$T/cpu/ivy.txt" "$T/cpu/v3")"; rc=$?
[ $rc = 0 ] && [ -z "$out" ]; check "the loader says supported: runs, whatever the flags say"
out="$(cpu_v3_missing "$T/cpu/v3.txt" "$T/cpu/notv3")"; rc=$?
[ $rc = 1 ] && has "$out" "its C library says so"; check "the loader says no: refused, though the flags look complete ($out)"
out="$(cpu_v3_missing "$T/cpu/ivy.txt" "$T/cpu/notv3")"; rc=$?
[ $rc = 1 ] && [ "$out" = "avx2 bmi1 bmi2 fma abm movbe" ]; check "the loader says no: the missing flags named ($out)"
out="$(cpu_v3_missing "$T/cpu/v3.txt" "$T/cpu/old")"; rc=$?
[ $rc = 0 ] && [ -z "$out" ]; check "a loader without glibc-hwcaps (glibc 2.31): the flags, complete"
out="$(cpu_v3_missing "$T/cpu/ivy.txt" "$T/cpu/old" "$T/cpu/missing-loader")"; rc=$?
[ $rc = 1 ] && [ "$out" = "avx2 bmi1 bmi2 fma abm movbe" ]; check "no loader answers, Ivy Bridge: refused ($out)"
out="$(cpu_v3_missing "$T/cpu/masked.txt" "$T/cpu/old")"; rc=$?
[ $rc = 1 ] && [ "$out" = "f16c abm" ]; check "no F16C, no LZCNT (the old check passed it): refused ($out)"
chmod -x "$T/cpu/v3"; cp "$T/cpu/v3" "$T/cpu/v3-runs"; chmod +x "$T/cpu/v3-runs"
out="$(cpu_v3_missing "$T/cpu/ivy.txt" "$T/cpu/v3" "$T/cpu/v3-runs")"; rc=$?
[ $rc = 0 ]; check "the bundled loader can't run (noexec): the system's answers"
out="$(cpu_v3_missing "$T/cpu/no-cpuinfo" "$T/cpu/old")"; rc=$?
[ $rc = 0 ] && [ -z "$out" ]; check "nothing to read: no refusal"

echo "glibc versions"
loader "$T/cpu/ubuntu" 2.39 -
[ "$(loader_version "$T/cpu/v3-runs")" = 2.41 ] && [ "$(loader_version "$T/cpu/ubuntu")" = 2.39 ]; check "a loader's version"
[ -z "$(loader_version "$T/cpu/v3")" ] && [ -z "$(loader_version "$T/cpu/nothing")" ]; check "a loader that can't run: none"

echo "the system's libstdc++ (host_cxx_lib, cxx_version)"
# cxxlib <file> <bits: 64 or 32> <e_machine: 3e00, b700 or 0300> <newest GLIBCXX_3.4.N>
cxxlib() {
    local class=02 n
    [ "$2" = 32 ] && class=01
    mkdir -p "$(dirname "$1")"
    printf "\\x7fELF\\x$class\\x01\\x01\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x03\\x00\\x${3:0:2}\\x${3:2:2}" > "$1"
    for n in $(seq 20 "$4"); do printf '\0GLIBCXX_3.4.%s' "$n" >> "$1"; done
}
R=$T/fedora
cxxlib $R/usr/lib/libstdc++.so.6 32 0300 35      # the 32-bit one, newer here: a wrong pick would show
cxxlib $R/usr/lib64/libstdc++.so.6 64 3e00 33
LDCONFIG=""
[ "$(host_cxx_lib $R)" = $R/usr/lib64/libstdc++.so.6 ]; check "Fedora (multilib, no ld.so.cache answer): /usr/lib64's, not /usr/lib's 32-bit one"
[ "$(cxx_version "$(host_cxx_lib $R)")" = GLIBCXX_3.4.33 ]; check "  its version"
R=$T/arch
cxxlib $R/usr/lib32/libstdc++.so.6 32 0300 35
cxxlib $R/usr/lib/libstdc++.so.6 64 3e00 34
LDCONFIG=$'\tlibstdc++.so.6 (libc6) => /usr/lib32/libstdc++.so.6\n\tlibstdc++.so.6 (libc6,x86-64) => /usr/lib/libstdc++.so.6'
[ "$(host_cxx_lib $R)" = $R/usr/lib/libstdc++.so.6 ]; check "Arch/SteamOS: ld.so.cache's x86-64 entry, /usr/lib"
R=$T/custom
cxxlib $R/opt/gcc/lib64/libstdc++.so.6 64 3e00 35
cxxlib $R/usr/lib64/libstdc++.so.6 64 3e00 30
LDCONFIG=$'\tlibstdc++.so.6 (libc6,x86-64) => /opt/gcc/lib64/libstdc++.so.6\n\tlibstdc++.so.6 (libc6,x86-64) => /usr/lib64/libstdc++.so.6'
[ "$(host_cxx_lib $R)" = $R/opt/gcc/lib64/libstdc++.so.6 ]; check "the loader's own pick (ld.so.cache) before the usual folders"
R=$T/fedora-cache
cxxlib $R/usr/lib/libstdc++.so.6 32 0300 35
cxxlib $R/usr/lib64/libstdc++.so.6 64 3e00 33
LDCONFIG=$'\tlibstdc++.so.6 (libc6) => /usr/lib/libstdc++.so.6\n\tlibstdc++.so.6 (libc6,x86-64) => /usr/lib64/libstdc++.so.6'
[ "$(host_cxx_lib $R)" = $R/usr/lib64/libstdc++.so.6 ]; check "Fedora with ld.so.cache: its x86-64 entry only"
R=$T/debian
cxxlib $R/usr/lib/i386-linux-gnu/libstdc++.so.6 32 0300 35
cxxlib $R/usr/lib/x86_64-linux-gnu/libstdc++.so.6 64 3e00 33
LDCONFIG=""
[ "$(host_cxx_lib $R)" = $R/usr/lib/x86_64-linux-gnu/libstdc++.so.6 ]; check "Debian/Ubuntu (multiarch)"
R=$T/arm
cxxlib $R/usr/lib64/libstdc++.so.6 64 3e00 35   # an x86-64 one where an arm64 machine has none
cxxlib $R/usr/lib/libstdc++.so.6 64 b700 34
FAKE_ARCH=aarch64
[ "$(host_cxx_lib $R)" = $R/usr/lib/libstdc++.so.6 ]; check "arm64: an arm64 library, not an x86-64 one"
FAKE_ARCH=x86_64
R=$T/none; mkdir -p $R/usr/lib
cxxlib $R/usr/lib/libstdc++.so.6 32 0300 35
host_cxx_lib $R > /dev/null; rc=$?
[ $rc = 1 ] && [ -z "$(host_cxx_lib $R)" ]; check "only a 32-bit one: none found"

echo "which runtime runs the game (use_bundled)"
[ "$(use_bundled 2.41 2.44)" = 1 ]; check "SteamOS 3.8's glibc 2.41, bundled 2.44: bundled"
[ "$(use_bundled 2.44 2.44)" = 0 ]; check "the same version: the system's"
[ "$(use_bundled 2.45 2.44)" = 0 ]; check "a newer system: the system's"
[ "$(use_bundled "" 2.44)" = 1 ]; check "no system loader found: bundled"
[ "$(use_bundled 2.41 "")" = 0 ]; check "the bundled one can't run: the system's"
[ "$(use_bundled GLIBCXX_3.4.9 GLIBCXX_3.4.10)" = 1 ]; check "GLIBCXX_3.4.9 is older than 3.4.10 (by version, not by text)"
[ "$(use_bundled 2.41 2.44 0)" = 0 ]; check "NFSMW_BUNDLED_*=0 over an older system"
[ "$(use_bundled 2.45 2.44 1)" = 1 ]; check "NFSMW_BUNDLED_*=1 over a newer system"

echo "the log's last place (private_log_dir)"
mkdir -p "$T/run" "$T/tmp"; chmod 700 "$T/run"
ln -s "$T/victim" "$T/tmp/last-run.log"   # the fixed name an attacker could plant in a shared /tmp
d="$(XDG_RUNTIME_DIR=$T/run private_log_dir)"
[ "$d" = "$T/run/speedbreaker" ] && [ -d "$d" ] && [ -O "$d" ]; check "XDG_RUNTIME_DIR: a folder of this user's in it"
d1="$(unset XDG_RUNTIME_DIR; TMPDIR=$T/tmp private_log_dir)"
d2="$(unset XDG_RUNTIME_DIR; TMPDIR=$T/tmp private_log_dir)"
[ -n "$d1" ] && [ -d "$d1" ] && [ "$d1" != "$d2" ] && [ "$(dirname "$d1")" = "$T/tmp" ] && [ "$(stat -c %a "$d1")" = 700 ]; check "no XDG_RUNTIME_DIR: a new private folder each run, not the shared folder itself"
rm -rf "$T/run/speedbreaker"; mkdir -p "$T/elsewhere"; ln -s "$T/elsewhere" "$T/run/speedbreaker"
d="$(XDG_RUNTIME_DIR=$T/run TMPDIR=$T/tmp private_log_dir)"
[ "$(dirname "$d")" = "$T/tmp" ]; check "a link in XDG_RUNTIME_DIR's place: not followed"
if [ "$(id -u)" = 0 ] && id nobody > /dev/null 2>&1; then
    mkdir -p "$T/others"; chown nobody "$T/others"
    d="$(XDG_RUNTIME_DIR=$T/others TMPDIR=$T/tmp private_log_dir)"
    [ "$(dirname "$d")" = "$T/tmp" ]; check "an XDG_RUNTIME_DIR of another user's: not used"
fi
[ ! -e "$T/victim" ]; check "the planted link untouched"

echo "the library folder (open_lib_dir)"
odd="$T/Need for Speed: Most Wanted;\$ORIGIN \$LIB"
mkdir -p "$odd/lib/cxx"
open_lib_dir "$odd/lib"; rc=$?
[ $rc = 0 ] && has "$LIB_DIR" /proc/self/fd/ && [ "$(readlink "$LIB_DIR")" = "$odd/lib" ] && [ -d "$LIB_DIR/cxx" ]; check "a folder with ':', ';' and '\$': named through a descriptor ($LIB_DIR)"
case "$LIB_DIR" in *[:\;\$]*) false ;; *) true ;; esac; check "  nothing the loader splits or expands left in it"
open_lib_dir "$T/no such: folder"; rc=$?
[ $rc = 1 ]; check "a folder that can't be opened, with a ':': refused"
open_lib_dir "$T/plain"; rc=$?
[ $rc = 0 ] && [ "$LIB_DIR" = "$T/plain" ]; check "a plain folder that can't be opened: its own path"

# The whole launcher, as Steam starts it: a bundle whose program is xz, in a
# folder named with ':', ';' and '$'. Before open_lib_dir the loader would
# split that folder's path and xz would load the system's liblzma (or none).
HOST_LOADER=/lib64/ld-linux-x86-64.so.2
XZ="$(command -v xz || true)"
LZMA=""
[ -n "$XZ" ] && LZMA="$(ldd "$XZ" 2>/dev/null | sed -n 's/^\s*\(liblzma\.so\.[0-9]*\) => \(\S*\).*/\1 \2/p')"
if [ "$(command uname -m)" != x86_64 ] || [ ! -x "$HOST_LOADER" ] || [ -z "$LZMA" ]; then
    echo "the whole launcher: skipped (needs x86-64, $HOST_LOADER and an xz linked with liblzma)"
elif ! cpu_v3_missing /proc/cpuinfo "$HOST_LOADER" > /dev/null; then
    echo "the whole launcher: skipped (this CPU isn't x86-64-v3, which the launcher refuses)"
else
    echo "the whole launcher"
    B="$T/Games/Need for Speed: Most Wanted;\$ORIGIN/SpeedBreaker"
    mkdir -p "$B/lib/cxx" "$T/home"
    cp "$LAUNCHER" "$B/speedbreaker.sh"
    echo "SpeedBreaker v0.0.0 (test)" > "$B/VERSION"
    cp "$XZ" "$B/lib/SpeedBreaker"
    soname="${LZMA%% *}"
    # run [VAR=value ...]: the launcher in a clean environment, LD_DEBUG=libs
    # (the loader names each library it starts); the log is last-run.log.
    run() { env -i PATH="$PATH" HOME="$T/home" LD_DEBUG=libs "$@" bash "$B/speedbreaker.sh" --version > /dev/null 2>&1; }
    cp -L "${LZMA#* }" "$B/lib/$soname"
    run; rc=$?
    log="$(cat "$B/last-run.log" 2>/dev/null)"
    [ $rc = 0 ] && has "$log" "xz (XZ Utils)"; check "it starts (exit $rc)"
    grep -qE "calling init: /proc/self/fd/[0-9]+/$soname" "$B/last-run.log"; check "  with lib/'s $soname, through the descriptor"
    has "$log" "C library: the system's" && has "$log" "C++ library: the system's"; check "  the system's C and C++ libraries (none bundled)"
    # lib/cxx too: the library there, a stand-in libstdc++ beside it (xz
    # needs none, so it is never loaded) and the bundled C++ runtime forced.
    mv "$B/lib/$soname" "$B/lib/cxx/$soname"
    printf '\0GLIBCXX_3.4.99' > "$B/lib/cxx/libstdc++.so.6"
    run NFSMW_BUNDLED_CXX=1; rc=$?
    log="$(cat "$B/last-run.log" 2>/dev/null)"
    [ $rc = 0 ] && has "$log" "xz (XZ Utils)" && has "$log" "C++ library: bundled"; check "lib/cxx: it starts (exit $rc)"
    grep -qE "calling init: /proc/self/fd/[0-9]+/cxx/$soname" "$B/last-run.log"; check "  with lib/cxx's $soname, through the descriptor"
    [ -s "$B/previous-run.log" ]; check "  the run before kept as previous-run.log"
    if [ $failures != 0 ]; then echo "---- last-run.log ----"; grep -v "^ *[0-9]*:" "$B/last-run.log"; fi
fi

echo
if [ $failures = 0 ]; then echo "all passed ($cases checks)"; else echo "$failures failure(s) in $cases checks"; fi
[ $failures = 0 ]
