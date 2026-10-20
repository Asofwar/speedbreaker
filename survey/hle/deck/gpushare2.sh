#!/bin/bash
# gpushare2.sh [seconds]: GPU time (amdgpu fdinfo drm-engine-gfx) of every
# process with a DRM client over the interval, as ms per second.
S="${1:-10}"
snap() {
    for p in /proc/[0-9]*; do
        for f in "$p"/fdinfo/*; do
            awk -v pid="${p#/proc/}" '/^drm-client-id:/ {id = $2} /^drm-engine-gfx:/ {g = $2} END {if (id != "" && g != "") print pid, id, g}' "$f" 2>/dev/null
        done
    done | sort -u
}
a=$(snap); t0=$(date +%s.%N)
sleep "$S"
b=$(snap); t1=$(date +%s.%N)
join -j 1 <(echo "$a" | awk '{print $1 "/" $2, $3}' | sort) <(echo "$b" | awk '{print $1 "/" $2, $3}' | sort) |
    awk -v dt="$(awk -v a="$t0" -v b="$t1" 'BEGIN {print b - a}')" '{split($1, k, "/"); d = ($3 - $2) / 1e6 / dt; if (d > 0.5) {cmd = "cat /proc/" k[1] "/comm"; cmd | getline name; close(cmd); printf "%-16s %7.1f ms/s\n", name, d}}'
