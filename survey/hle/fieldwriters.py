#!/usr/bin/env python3
# fieldwriters.py <lo_off> <hi_off> [ld]: functions (anywhere) that store (or,
# with "ld", load) at a displacement in [lo_off, hi_off) from any base
# register: candidates for code touching a device field. Prints function,
# count and the displacements.
import pickle, re, sys, collections
funcs, insns, calls, callees = pickle.load(open('/work/hle/xref.pkl', 'rb'))
lo, hi = int(sys.argv[1]), int(sys.argv[2])
loads = len(sys.argv) > 3 and sys.argv[3] == 'ld'
rx = re.compile(r'^(lwz|lbz|lhz|ld|lfs|lfd|lha|lwzu|lbzu|lhzu|ldu)\s+\S+,(-?\d+)\((r\d+)\)$' if loads else
                r'^(stw|stb|sth|std|stfs|stfd|stwu|stbu|sthu|stdu)\s+\S+,(-?\d+)\((r\d+)\)$')
for fn in funcs:
    offs = collections.Counter()
    for a, t in insns[fn]:
        m = rx.match(t.strip())
        if m and m.group(3) != 'r1':
            o = int(m.group(2))
            if lo <= o < hi:
                offs[o] += 1
    if offs:
        print(f'sub_{fn:08X} ({len(calls.get(fn, []))} callers): ' + ' '.join(f'{o}' + (f'x{n}' if n > 1 else '') for o, n in sorted(offs.items())))
