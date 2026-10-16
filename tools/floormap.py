"""Render one area of a decoded dungeon chunk 0 as text: '#' solid, '-|_|' wall-edge cells (types 1-4), 'S' stairs (0x28nn),
'D' doors (0xC8/0xCA/0xCC), 'N' NPC/staff markers (0x2C), '.' floor; optionally mark a cell with '@' and list the stairs ids.

usage: floormap.py <chunk0.bin> <area> [col,row]"""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
area = int(sys.argv[2])
mark = tuple(map(int, sys.argv[3].split(','))) if len(sys.argv) > 3 else None
o = 0x34 + 40 * area
w, h = struct.unpack_from('<HH', d, o)
offs = struct.unpack_from('<7I', d, o + 12)
W, H = 2 * w, 2 * h
C = [struct.unpack_from('<H', d, offs[2] + 2 * i)[0] for i in range(W * H)]
B = [struct.unpack_from('<H', d, offs[1] + 2 * i)[0] for i in range(W * H)]
print('   ' + ''.join(str(c % 10) for c in range(W)))
ids = {}
for r in range(H):
    s = ''
    for c in range(W):
        v = C[r * W + c] & 0xFF
        b = B[r * W + c]
        if mark == (c, r):
            s += '@'
        elif b >> 8 in (0xC8, 0xCA, 0xCC):
            s += 'D'
        elif b >> 8 == 0x28:
            s += 'S'
            ids.setdefault(b & ~1, []).append((c, r))
        elif b >> 8 == 0x2C:
            s += 'N'
        elif v in (0x3B, 0x3D):
            s += '#'
        elif 1 <= v <= 4:
            s += '-|_|'[v - 1]
        else:
            s += '.'
    print(f'{r:2d} {s}')
for k, v in sorted(ids.items()):
    print(f'stairs {k:04x}: {v}')
