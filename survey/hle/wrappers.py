#!/usr/bin/env python3
# wrappers.py <lo> <hi>: functions in [lo, hi) with their direct D3D-range
# callees, caller counts and indirect call counts.
import bisect, pickle, sys
funcs, insns, calls, callees = pickle.load(open('/work/hle/xref.pkl', 'rb'))
D3D_LO, D3D_HI = 0x82587000, 0x825B0000
lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
only = len(sys.argv) > 3 and sys.argv[3] == '--d3d'
i = bisect.bisect_left(funcs, lo)
while i < len(funcs) and funcs[i] < hi:
    fn = funcs[i]
    i += 1
    d3d = [f'sub_{t:08X}' for t, a, k in callees.get(fn, []) if D3D_LO <= t < D3D_HI]
    if only and not d3d:
        continue
    nc = len(calls.get(fn, []))
    icall = sum(1 for a, t in insns[fn] if t.startswith('bctrl'))
    ic = f' bctrl x{icall}' if icall else ''
    print(f'sub_{fn:08X} {len(insns[fn]):4d} insns, {nc:3d} callers; d3d: {" ".join(d3d)}{ic}')
