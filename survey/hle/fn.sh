#!/bin/bash
# fn.sh <hex addr>...: print functions from /work/hle/dis.txt
for a in "$@"; do
    a=$(echo "$a" | tr a-f A-F | sed 's/^SUB_//; s/^0X//')
    awk -v n="== sub_$a" 'BEGIN{p=0} /^== /{ if (p) exit; if ($0 == n) p=1 } p' /work/hle/dis.txt
done
