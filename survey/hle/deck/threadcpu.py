#!/usr/bin/env python3
# threadcpu.py <pid>: one line per thread of the process, "tid comm ticks"
# (utime + stime, CLK_TCK ticks), then "process ticks" and "system ticks"
# (every CPU's non-idle ticks from /proc/stat). Diff two snapshots to get
# the CPU time between them (profsum.py).
import os, sys
pid = sys.argv[1]
total = 0
for tid in sorted(os.listdir(f'/proc/{pid}/task'), key=int):
    try:
        stat = open(f'/proc/{pid}/task/{tid}/stat').read()
    except OSError:
        continue
    comm = stat[stat.index('(') + 1:stat.rindex(')')]
    f = stat[stat.rindex(')') + 2:].split()
    ticks = int(f[11]) + int(f[12])
    total += ticks
    print(f'{tid} {comm.replace(" ", "_")} {ticks}')
print(f'process - {total}')
c = open('/proc/stat').readline().split()[1:]
busy = sum(int(x) for x in c) - int(c[3]) - int(c[4])
print(f'system - {busy}')
