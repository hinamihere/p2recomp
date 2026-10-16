"""List the layer-B markers of a decoded dungeon/field chunk 0 (see tools/p2lz.py) per area, in 256-unit cells.

usage: fmarkers.py <chunk0.bin> [area]
Marker kinds seen so far (Zodiac/Kasugayama): 0xCA0n / 0xCC0n room doors, 0x2800/01 stairs down, 0x2802/03 stairs up,
0x2c0n staffers / guards, 0x2001 event lines, 0x0001 chests (layer C 0xd0), 0xe00n / 0xb00n other props.
The player's cell is the byte pair 08CEB2E0 (col) / 08CEB2E4 (row)."""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
areas = [int(sys.argv[2])] if len(sys.argv) > 2 else range(8)
for a in areas:
    o = 0x34 + 40 * a
    w, h = struct.unpack_from('<HH', d, o)
    if not 0 < w < 200 or not 0 < h < 200:
        continue
    offs = struct.unpack_from('<7I', d, o + 12)
    W, H = 2 * w, 2 * h
    B = [struct.unpack_from('<H', d, offs[1] + 2 * i)[0] for i in range(W * H)]
    groups = {}
    for i, v in enumerate(B):
        if v in (0, 0x8000) or v >> 12 == 4:  # 0x4xxx = region tags, not markers
            continue
        groups.setdefault(v, []).append((i % W, i // W))
    print(f'area {a}: {W}x{H} cells')
    for v, cells in sorted(groups.items()):
        print(f'  {v:04x}: {cells[:12]}{" ..." if len(cells) > 12 else ""}')
