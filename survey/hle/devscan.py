#!/usr/bin/env python3
# devscan.py: how code outside the D3D library uses the D3D device.
# Tracks registers holding the device pointer (loaded from the global at
# DEVGLOBAL, copied with mr; volatile registers dropped at calls) and reports
#   - accesses to device fields by offset region,
#   - indirect calls through device tables (lwz rX,off(dev); mtctr; bctrl),
#   - direct calls into the D3D range (all call sites, with or without device).
# Usage: devscan.py summary | fields | calls | indirect | funcs
import pickle, re, sys, collections

DEVGLOBAL = 0x82A2CF60
D3D_LO, D3D_HI = 0x82587000, 0x825B0000
with open('/work/hle/xref.pkl', 'rb') as f:
    funcs, insns, calls, callees = pickle.load(f)

REGIONS = [
    (0, 4, 'ring ptr'), (4, 16, 'ring misc'), (16, 56, 'dirty masks'), (56, 96, 'hdr'),
    (96, 484, 'SetRenderState table'), (484, 564, 'SetSamplerState table'),
    (564, 952, 'GetRenderState table'), (952, 1032, 'GetSamplerState table'),
    (1032, 1152, 'misc0'), (1152, 1920, 'fetch const shadow'), (1920, 6016, 'VS float const shadow'),
    (6016, 10112, 'PS float const shadow'), (10112, 10272, 'bool/loop shadow'),
    (10272, 11456, 'misc1'), (11456, 12288, 'register shadow'), (12288, 0x10000, 'misc2'),
]
def region(off):
    for lo, hi, n in REGIONS:
        if lo <= off < hi:
            return n
    return 'other'

rx_lis = re.compile(r'^lis (r\d+),(-?\d+)$')
rx_ld = re.compile(r'^(lwz|lbz|lhz|ld|lfs|lfd|lha|lwa|lwzu|lbzu|lhzu|ldu)\s+(r\d+|f\d+),(-?\d+)\((r\d+)\)$')
rx_st = re.compile(r'^(stw|stb|sth|std|stfs|stfd|stwu|stbu|sthu|stdu)\s+(r\d+|f\d+),(-?\d+)\((r\d+)\)$')
rx_mr = re.compile(r'^mr (r\d+),(r\d+)$')
rx_addi = re.compile(r'^addi (r\d+),(r\d+),(-?\d+)$')
rx_dst = re.compile(r'^\S+\.? (r\d+),')
VOLATILE = {f'r{i}' for i in range(0, 13)}

def scan(fn):
    lis = {}
    dev = {}       # reg -> offset from device base (0 = device ptr itself)
    tabload = {}   # reg -> device offset it was loaded from (for bctrl)
    ctr = None
    out = []       # (addr, kind, off)
    for a, t in insns[fn]:
        t = t.strip()
        m = rx_lis.match(t)
        if m:
            r = m.group(1)
            lis[r] = (int(m.group(2)) << 16) & 0xFFFFFFFF
            dev.pop(r, None); tabload.pop(r, None)
            continue
        m = rx_ld.match(t)
        if m:
            op, rd, off, rb = m.group(1), m.group(2), int(m.group(3)), m.group(4)
            if rb in lis and ((lis[rb] + off) & 0xFFFFFFFF) == DEVGLOBAL and op == 'lwz':
                dev[rd] = 0
                tabload.pop(rd, None)
                out.append((a, 'getdev', 0))
                continue
            if rb in dev:
                o = dev[rb] + off
                out.append((a, 'load', o))
                if rd.startswith('r'):
                    dev.pop(rd, None)
                    lis.pop(rd, None)
                    tabload[rd] = o
                continue
            if rd.startswith('r'):
                dev.pop(rd, None); lis.pop(rd, None); tabload.pop(rd, None)
            continue
        m = rx_st.match(t)
        if m:
            rb, off = m.group(4), int(m.group(3))
            if rb in dev:
                out.append((a, 'store', dev[rb] + off))
            continue
        m = rx_mr.match(t)
        if m:
            rd, rs = m.group(1), m.group(2)
            if rs in dev:
                dev[rd] = dev[rs]
            else:
                dev.pop(rd, None)
            if rs in tabload:
                tabload[rd] = tabload[rs]
            else:
                tabload.pop(rd, None)
            lis.pop(rd, None)
            continue
        m = rx_addi.match(t)
        if m:
            rd, rs, imm = m.group(1), m.group(2), int(m.group(3))
            if rs in dev:
                dev[rd] = dev[rs] + imm
                lis.pop(rd, None); tabload.pop(rd, None)
                continue
            if rs in lis:
                lis[rd] = (lis[rs] + imm) & 0xFFFFFFFF
                dev.pop(rd, None); tabload.pop(rd, None)
                continue
        if t.startswith('mtctr '):
            r = t.split()[1]
            ctr = tabload.get(r)
            continue
        if t.startswith('bctrl'):
            if ctr is not None:
                out.append((a, 'icall', ctr))
            ctr = None
            for r in VOLATILE:
                dev.pop(r, None); lis.pop(r, None); tabload.pop(r, None)
            continue
        if t.startswith('bl '):
            tgt = int(t.split()[1], 16)
            out.append((a, 'call', tgt if 'r3' in dev and dev['r3'] == 0 else -tgt))
            for r in VOLATILE:
                dev.pop(r, None); lis.pop(r, None); tabload.pop(r, None)
            continue
        m = rx_dst.match(t)
        if m and not t.startswith(('cmp', 'std', 'stw', 'stb', 'sth', 'stf', 'stv', 'dcb', 'tw', 'td')):
            r = m.group(1)
            dev.pop(r, None); lis.pop(r, None); tabload.pop(r, None)
    return out

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'summary'
    fields = collections.Counter()
    fieldfuncs = collections.defaultdict(set)
    icalls = collections.Counter()
    icallfuncs = collections.defaultdict(set)
    devcalls = collections.Counter()
    alld3dcalls = collections.Counter()
    alld3dfuncs = collections.defaultdict(set)
    users = set()
    for fn in funcs:
        if D3D_LO <= fn < D3D_HI:
            continue
        res = scan(fn)
        for a, k, o in res:
            if k in ('load', 'store'):
                fields[(region(o), k)] += 1
                fieldfuncs[(region(o), k)].add(fn)
                users.add(fn)
                if mode == 'fields':
                    print(f'sub_{fn:08X} {a:08X} {k} +{o} ({region(o)})')
            elif k == 'icall':
                icalls[o] += 1
                icallfuncs[o].add(fn)
                users.add(fn)
            elif k == 'call':
                if o > 0:
                    devcalls[o] += 1
        for tgt, a, kind in callees.get(fn, []):
            if D3D_LO <= tgt < D3D_HI:
                alld3dcalls[tgt] += 1
                alld3dfuncs[tgt].add(fn)
    if mode == 'summary':
        print('device field accesses outside D3D (sites, functions):')
        for key in sorted(fields, key=lambda k: -fields[k]):
            print(f'  {key[0]:24s} {key[1]:5s} {fields[key]:6d} sites in {len(fieldfuncs[key]):5d} funcs')
        print(f'functions using the device global: {len(users)}')
        print('indirect calls through device tables (offset: sites, funcs):')
        for o in sorted(icalls):
            print(f'  +{o} ({region(o)}; slot {(o - 96) // 4 if 96 <= o < 484 else (o - 484) // 4 if 484 <= o < 564 else "-"}): {icalls[o]} sites in {len(icallfuncs[o])} funcs')
        print(f'direct calls into D3D range [{D3D_LO:08X},{D3D_HI:08X}) from outside: {sum(alld3dcalls.values())} sites, {len(alld3dcalls)} callees')
        for t in sorted(alld3dcalls, key=lambda t: -alld3dcalls[t]):
            print(f'  sub_{t:08X}: {alld3dcalls[t]} sites in {len(alld3dfuncs[t])} funcs')
    if mode == 'funcs':
        for t in sorted(alld3dcalls):
            print(f'sub_{t:08X}: ' + ' '.join(f'sub_{f:08X}' for f in sorted(alld3dfuncs[t])))

main()
