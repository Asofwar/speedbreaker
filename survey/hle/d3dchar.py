#!/usr/bin/env python3
# d3dchar.py <addr>...: characterise D3D functions: device fields read and
# written (r3 at entry taken as the device), PM4 headers built, callees.
import pickle, re, sys, collections
sys.path.insert(0, '/work/hle/tools')
funcs, insns, calls, callees = pickle.load(open('/work/hle/xref.pkl', 'rb'))

rx_ld = re.compile(r'^(lwz|lbz|lhz|ld|lfs|lfd|lha|lwzu|lbzu|lhzu|ldu|lwzx)\s+(r\d+|f\d+),(-?\d+)\((r\d+)\)$')
rx_st = re.compile(r'^(stw|stb|sth|std|stfs|stfd|stwu|stbu|sthu|stdu)\s+(r\d+|f\d+),(-?\d+)\((r\d+)\)$')
rx_mr = re.compile(r'^mr (r\d+),(r\d+)$')
rx_addi = re.compile(r'^addi (r\d+),(r\d+),(-?\d+)$')
rx_dst = re.compile(r'^\S+\.? (r\d+),')
VOL = {f'r{i}' for i in range(0, 13)}

def char(fn):
    dev = {'r3': 0}
    rd_, wr_ = collections.Counter(), collections.Counter()
    for a, t in insns[fn]:
        t = t.strip()
        m = rx_ld.match(t)
        if m:
            d, off, b = m.group(2), int(m.group(3)), m.group(4)
            if b in dev:
                rd_[dev[b] + off] += 1
            if d.startswith('r'):
                dev.pop(d, None)
            continue
        m = rx_st.match(t)
        if m:
            off, b = int(m.group(3)), m.group(4)
            if b in dev:
                wr_[dev[b] + off] += 1
            continue
        m = rx_mr.match(t)
        if m:
            if m.group(2) in dev:
                dev[m.group(1)] = dev[m.group(2)]
            else:
                dev.pop(m.group(1), None)
            continue
        m = rx_addi.match(t)
        if m and m.group(2) in dev:
            dev[m.group(1)] = dev[m.group(2)] + int(m.group(3))
            continue
        if t.startswith('bl ') or t.startswith('bctrl'):
            for r in VOL:
                dev.pop(r, None)
            continue
        m = rx_dst.match(t)
        if m and not t.startswith(('cmp', 'st', 'dcb', 'tw', 'td')):
            dev.pop(m.group(1), None)
    return rd_, wr_

def fmt(c):
    return ' '.join(f'{o}' + (f'x{n}' if n > 1 else '') for o, n in sorted(c.items()))

for s in sys.argv[1:]:
    fn = int(s.lower().replace('sub_', ''), 16)
    r, w = char(fn)
    cs = ' '.join(f'sub_{t:08X}' for t, a, k in callees.get(fn, []))
    ncall = len(calls.get(fn, []))
    print(f'sub_{fn:08X} ({len(insns[fn])} insns, {ncall} call sites)')
    print(f'   reads dev+: {fmt(r)}')
    print(f'   writes dev+: {fmt(w)}')
    print(f'   calls: {cs}')
