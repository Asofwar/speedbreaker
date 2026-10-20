#!/usr/bin/env python3
# mkdis.py <ppc dir> <out.txt>: rebuild a disassembly listing ("ADDR: insn")
# from XenonRecomp's generated C++ (each instruction is preceded by a
# "// insn" comment). Addresses are counted from each function's start and
# checked against "ctx.lr = 0x..." after calls and "loc_XXXXXXXX:" labels.
import os, re, sys

src, out = sys.argv[1], sys.argv[2]
func_re = re.compile(r'^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)')
loc_re = re.compile(r'^loc_([0-9A-F]{8}):')
lr_re = re.compile(r'^\tctx\.lr = 0x([0-9A-F]{8});')
com_re = re.compile(r'^\t// (.*)$')

files = sorted(f for f in os.listdir(src) if f.startswith('ppc_recomp.') and f.endswith('.cpp'))
mism = 0
nins = 0
with open(out, 'w') as o:
    for fn in files:
        cur = None
        addr = 0
        last = None
        with open(os.path.join(src, fn), errors='replace') as f:
            for line in f:
                m = func_re.match(line)
                if m:
                    cur = int(m.group(1), 16)
                    addr = cur
                    last = None
                    o.write(f'== sub_{cur:08X}\n')
                    continue
                if cur is None:
                    continue
                m = loc_re.match(line)
                if m:
                    a = int(m.group(1), 16)
                    if a != addr:
                        mism += 1
                        if mism < 20:
                            print(f'label mismatch {fn} sub_{cur:08X}: loc {a:08X} counted {addr:08X}', file=sys.stderr)
                        addr = a
                    continue
                m = com_re.match(line)
                if m:
                    txt = m.group(1)
                    o.write(f'{addr:08X}: {txt}\n')
                    last = addr
                    addr += 4
                    nins += 1
                    continue
                m = lr_re.match(line)
                if m and last is not None:
                    a = int(m.group(1), 16)
                    if a != last + 4:
                        mism += 1
                        if mism < 20:
                            print(f'lr mismatch {fn} sub_{cur:08X}: lr {a:08X} insn {last:08X}', file=sys.stderr)
print(f'{nins} instructions, {mism} mismatches', file=sys.stderr)
