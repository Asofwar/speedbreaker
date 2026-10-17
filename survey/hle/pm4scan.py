#!/usr/bin/env python3
# pm4scan.py: find 32-bit constants each function builds (lis + ori/addi/oris
# on a register, li) that look like PM4 packet headers, and the stores
# of registers holding them. Output per function: the headers it builds.
#   pm4scan.py [lo hi]          summary per function
#   pm4scan.py --op 2D          functions building type-3 opcode 0x2D
import pickle, re, sys, collections

with open('/work/hle/xref.pkl', 'rb') as f:
    funcs, insns, calls, callees = pickle.load(f)

OPS = {
    0x10: 'NOP', 0x21: 'REG_RMW', 0x22: 'DRAW_INDX', 0x23: 'VIZ_QUERY', 0x26: 'WAIT_FOR_IDLE',
    0x27: 'IM_LOAD', 0x2B: 'IM_LOAD_IMMEDIATE', 0x2D: 'SET_CONSTANT', 0x2F: 'LOAD_ALU_CONSTANT',
    0x36: 'DRAW_INDX_2', 0x37: 'INDIRECT_BUFFER_PFD', 0x3B: 'INVALIDATE_STATE', 0x3C: 'WAIT_REG_MEM',
    0x3D: 'MEM_WRITE', 0x3E: 'REG_TO_MEM', 0x3F: 'INDIRECT_BUFFER', 0x44: 'CP_INTERRUPT?', 0x45: 'COND_WRITE',
    0x46: 'EVENT_WRITE', 0x48: 'ME_INIT', 0x50: 'SET_BIN_MASK', 0x51: 'SET_BIN_SELECT', 0x54: 'INTERRUPT',
    0x55: 'SET_CONSTANT2', 0x56: 'SET_SHADER_CONSTANTS', 0x58: 'EVENT_WRITE_SHD', 0x5A: 'EVENT_WRITE_EXT',
    0x5B: 'EVENT_WRITE_ZPD', 0x5E: 'CONTEXT_UPDATE', 0x60: 'SET_BIN_MASK_LO', 0x61: 'SET_BIN_MASK_HI',
    0x62: 'SET_BIN_SELECT_LO', 0x63: 'SET_BIN_SELECT_HI', 0x64: 'XE_SWAP',
}

rx_lis = re.compile(r'^lis (r\d+),(-?\d+)$')
rx_li = re.compile(r'^li (r\d+),(-?\d+)$')
rx_ori = re.compile(r'^ori (r\d+),(r\d+),(\d+)$')
rx_oris = re.compile(r'^oris (r\d+),(r\d+),(\d+)$')
rx_addi = re.compile(r'^addi (r\d+),(r\d+),(-?\d+)$')

def consts(fn):
    """Yield (addr, value) for every 32-bit constant materialised in fn."""
    known = {}
    for a, t in insns[fn]:
        t = t.strip()
        m = rx_lis.match(t)
        if m:
            known[m.group(1)] = (int(m.group(2)) << 16) & 0xFFFFFFFF
            continue
        m = rx_li.match(t)
        if m:
            v = int(m.group(2)) & 0xFFFFFFFF
            known[m.group(1)] = v
            yield a, v, 'li'
            continue
        m = rx_ori.match(t)
        if m and m.group(2) in known:
            v = known[m.group(2)] | int(m.group(3))
            known[m.group(1)] = v
            yield a, v, 'ori'
            continue
        m = rx_oris.match(t)
        if m and m.group(2) in known:
            v = known[m.group(2)] | (int(m.group(3)) << 16)
            known[m.group(1)] = v
            yield a, v, 'oris'
            continue
        m = rx_addi.match(t)
        if m and m.group(2) in known and m.group(2) != 'r1':
            v = (known[m.group(2)] + int(m.group(3))) & 0xFFFFFFFF
            known[m.group(1)] = v
            yield a, v, 'addi'
            continue
        # any other write to a register forgets it (rough: first operand)
        mm = re.match(r'^\S+\.? (r\d+),', t)
        if mm and not t.startswith(('stw', 'stb', 'sth', 'std', 'cmp', 'stfs', 'stfd', 'stv')):
            known.pop(mm.group(1), None)

T0 = False
def classify(v, kind):
    t = v >> 30
    if t == 3:
        op = (v >> 8) & 0x7F
        cnt = ((v >> 16) & 0x3FFF) + 1
        if op in OPS and (v & 0xFE) == 0 and cnt < 0x1000:
            return f'T3 {OPS[op]}{"(pred)" if v & 1 else ""} n={cnt}'
    if t == 0 and kind == 'ori' and T0:
        cnt = ((v >> 16) & 0x3FFF) + 1
        reg = v & 0x7FFF
        if (0x2000 <= reg < 0x2400 or 0x4000 <= reg < 0x4930 or 0x0A00 <= reg < 0x0A40 or 0x0500 <= reg < 0x0600) and cnt <= 0x400 and v >= 0x10000:
            return f'T0 {reg:04X} n={cnt}'
    return None

def main():
    global T0
    args = sys.argv[1:]
    T0 = '--t0' in args
    args = [a for a in args if a != '--t0']
    want = None
    lo, hi = 0, 0xFFFFFFFF
    if args and args[0] == '--op':
        want = int(args[1], 16)
        args = args[2:]
    if len(args) >= 2:
        lo, hi = int(args[0], 16), int(args[1], 16)
    for fn in funcs:
        if fn < lo or fn >= hi:
            continue
        found = collections.OrderedDict()
        for a, v, kind in consts(fn):
            if kind == 'li' or kind == 'addi':
                continue
            c = classify(v, kind)
            if c:
                if want is not None and ((v >> 8) & 0x7F) != want:
                    continue
                found.setdefault(c, []).append(a)
        if found:
            print(f'sub_{fn:08X}: ' + '; '.join(f'{k} @{",".join(f"{x:08X}" for x in v[:3])}' for k, v in found.items()))

main()
