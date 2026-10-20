#!/usr/bin/env python3
# apupower.py [seconds] [interval]: the Steam Deck APU's power split from
# amdgpu's gpu_metrics (v2.x, Van Gogh; readable without root): socket, CPU
# cores, SoC and GFX power (mW in the table, printed as W), the GFX clock and
# activity; the CPU cores' mean clock from cpufreq (the table's is empty). Prints one line per sample and a mean at
# the end ("mean" line). Python 3 standard library only.
import struct, sys, time
F = '/sys/class/drm/card0/device/gpu_metrics'
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0
step = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5

def sample():
    b = open(F, 'rb').read()
    size, fmt, content = struct.unpack_from('<HBB', b, 0)
    if fmt != 2:
        raise SystemExit(f'gpu_metrics format {fmt}.{content}: not a v2 APU table')
    gfx_act, = struct.unpack_from('<H', b, 28)
    socket, cpu, soc, gfx = struct.unpack_from('<4H', b, 40)
    gfxclk, = struct.unpack_from('<H', b, 64)
    cores = [int(open(f'/sys/devices/system/cpu/cpu{i}/cpufreq/scaling_cur_freq').read()) // 1000 for i in range(8)]
    return dict(socket=socket / 1000, cpu=cpu / 1000, soc=soc / 1000, gfx=gfx / 1000, gfxclk=gfxclk,
                gfx_act=gfx_act / 100, cpu_mhz=sum(cores) / 8)

rows = []
end = time.time() + secs
while time.time() < end:
    r = sample()
    rows.append(r)
    print(' '.join(f'{k} {v:.2f}' if isinstance(v, float) else f'{k} {v}' for k, v in r.items()), flush=True)
    time.sleep(step)
if rows:
    m = {k: sum(r[k] for r in rows) / len(rows) for k in rows[0]}
    print('mean ' + ' '.join(f'{k} {v:.2f}' for k, v in m.items()) + f' ({len(rows)} samples)')
