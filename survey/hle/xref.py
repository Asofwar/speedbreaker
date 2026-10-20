#!/usr/bin/env python3
# xref.py: queries over /work/hle/dis.txt.
#   xref.py callers <addr>...      functions calling (bl / b) each address
#   xref.py callees <addr>         what a function calls
#   xref.py range <lo> <hi>        functions in [lo, hi) with size and caller count
#   xref.py grep <regex> [lo hi]   matching instructions with their function
import bisect, os, pickle, re, sys

DIS = '/work/hle/dis.txt'
DB = '/work/hle/xref.pkl'

def build():
    funcs = []          # start addresses
    insns = {}          # func -> list of (addr, text)
    cur = None
    with open(DIS) as f:
        for line in f:
            if line.startswith('== sub_'):
                cur = int(line[7:15], 16)
                funcs.append(cur)
                insns[cur] = []
            else:
                a = int(line[:8], 16)
                insns[cur].append((a, line[10:].rstrip('\n')))
    funcs.sort()
    calls = {}          # callee -> list of (caller func, insn addr, kind)
    callees = {}
    br = re.compile(r'^(bl|b)\s+0x([0-9a-f]{8})')
    starts = set(funcs)
    for fn in funcs:
        lst = []
        for a, t in insns[fn]:
            m = br.match(t)
            if not m:
                continue
            tgt = int(m.group(2), 16)
            if m.group(1) == 'b' and tgt not in starts:
                continue  # local branch
            if m.group(1) == 'b' and tgt == fn:
                continue
            calls.setdefault(tgt, []).append((fn, a, m.group(1)))
            lst.append((tgt, a, m.group(1)))
        callees[fn] = lst
    with open(DB, 'wb') as f:
        pickle.dump((funcs, insns, calls, callees), f)
    return funcs, insns, calls, callees

def load():
    if os.path.exists(DB) and os.path.getmtime(DB) > os.path.getmtime(DIS):
        with open(DB, 'rb') as f:
            return pickle.load(f)
    return build()

def h(s):
    return int(s.lower().replace('sub_', '').replace('0x', ''), 16)

def main():
    funcs, insns, calls, callees = load()
    cmd = sys.argv[1]
    if cmd == 'callers':
        for s in sys.argv[2:]:
            a = h(s)
            lst = calls.get(a, [])
            fs = sorted(set(c[0] for c in lst))
            print(f'sub_{a:08X}: {len(lst)} call sites in {len(fs)} functions')
            for fn in fs:
                sites = [f'{c[1]:08X}{"(b)" if c[2] == "b" else ""}' for c in lst if c[0] == fn]
                print(f'  sub_{fn:08X} ({len(insns[fn])} insns) at {" ".join(sites)}')
    elif cmd == 'callees':
        a = h(sys.argv[2])
        for tgt, at, k in callees.get(a, []):
            print(f'  {at:08X} {k} sub_{tgt:08X}')
    elif cmd == 'range':
        lo, hi = h(sys.argv[2]), h(sys.argv[3])
        i = bisect.bisect_left(funcs, lo)
        while i < len(funcs) and funcs[i] < hi:
            fn = funcs[i]
            lst = calls.get(fn, [])
            ext = len(set(c[0] for c in lst))
            print(f'sub_{fn:08X} {len(insns[fn]):5d} insns, {len(lst):4d} sites in {ext:4d} funcs')
            i += 1
    elif cmd == 'grep':
        rx = re.compile(sys.argv[2])
        lo = h(sys.argv[3]) if len(sys.argv) > 3 else 0
        hi = h(sys.argv[4]) if len(sys.argv) > 4 else 0xFFFFFFFF
        for fn in funcs:
            if fn < lo or fn >= hi:
                continue
            for a, t in insns[fn]:
                if rx.search(t):
                    print(f'sub_{fn:08X} {a:08X}: {t}')

main()
