#!/usr/bin/env python3
# xexlibs.py <default.xex>: the XEX2 optional header 0x000200FF (static
# libraries the executable was linked with, and their XDK versions).
import struct, sys
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'XEX2', d[:4]
count = struct.unpack('>I', d[0x14:0x18])[0]
for i in range(count):
    key, val = struct.unpack('>II', d[0x18 + i * 8:0x20 + i * 8])
    if key == 0x000200FF:
        size = struct.unpack('>I', d[val:val + 4])[0]
        o = val + 4
        while o + 16 <= val + size:
            name = d[o:o + 8].rstrip(b'\0').decode(errors='replace')
            major, minor, build, qfe = struct.unpack('>HHHH', d[o + 8:o + 16])
            print(f'{name:8s} {major}.{minor}.{build}.{qfe & 0xFF} (approval {qfe >> 13})')
            o += 16
    elif key == 0x00018102 or key == 0x00040006:
        pass
