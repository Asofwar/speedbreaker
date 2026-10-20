#!/usr/bin/env python3
# syscpu.py [seconds]: every process's CPU time over the interval, the top
# 12 by ms of CPU a second, and the system's non-idle total (CLK_TCK = 100).
import os, sys, time
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0

def snap():
    d = {}
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        try:
            stat = open(f'/proc/{pid}/stat').read()
        except OSError:
            continue
        comm = stat[stat.index('(') + 1:stat.rindex(')')]
        f = stat[stat.rindex(')') + 2:].split()
        d[pid] = (comm, int(f[11]) + int(f[12]))
    c = open('/proc/stat').readline().split()[1:]
    busy = sum(int(x) for x in c) - int(c[3]) - int(c[4])
    return d, busy
a, ba = snap()
time.sleep(secs)
b, bb = snap()
rows = sorted(((b[p][0], (b[p][1] - a[p][1]) * 10.0 / secs) for p in b if p in a), key=lambda x: -x[1])
print(f'system busy {(bb - ba) * 10.0 / secs:.0f} ms/s over {secs:.0f} s')
for comm, ms in rows[:12]:
    if ms >= 1:
        print(f'  {comm:24s} {ms:7.1f} ms/s')
