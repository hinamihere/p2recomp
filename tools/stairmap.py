"""Map where each staircase of a dungeon floor leads: for every layer-B stairs marker pair (0x28nn) on <area>, walk to a free
neighbouring cell (tools/cellwalk.py), push into the stairs (tools/stairpush.py) and record the floor and cell the party lands on.

usage: stairmap.py <chunk0.bin> <area> <from-slot> <first-out-slot> [stair-id ...]
Prints one line per staircase: id, cells, landing (floor, cell) and the savestate slot of the landing (each try uses 60 slots)."""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
chunk, area, src, out = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
only = {int(x, 16) for x in sys.argv[5:]}
d = open(chunk, 'rb').read()
o = 0x34 + 40 * area
w, h = struct.unpack_from('<HH', d, o)
offs = struct.unpack_from('<7I', d, o + 12)
W, H = 2 * w, 2 * h
B = [struct.unpack_from('<H', d, offs[1] + 2 * i)[0] for i in range(W * H)]
C = [struct.unpack_from('<H', d, offs[2] + 2 * i)[0] & 0xFF for i in range(W * H)]
EDGES = os.environ.get('P2_CELLWALK_EDGES_SOLID') == '1'
solid = lambda c, r: (not (0 <= c < W and 0 <= r < H) or C[r * W + c] in (0x3B, 0x3D) or B[r * W + c] >> 8 == 0x28
                      or (EDGES and 1 <= C[r * W + c] <= 4))
stairs = {}
for i, v in enumerate(B):
    if v >> 8 == 0x28:
        stairs.setdefault(v & ~1, []).append((i % W, i // W))


def st(s):
    with open(os.path.join(ROOT, 'states', f'slot{s}.p2s'), 'rb') as f:
        f.seek(24)
        r = f.read(0x2000000)
    return (r[0xCEB2E0], r[0xCEB2E4]), r[0xCE819D]


def latest(lo, hi):
    import glob
    c = [f for f in glob.glob(os.path.join(ROOT, 'states', 'slot*.p2s'))
         if os.path.basename(f)[4:-4].isdigit() and lo <= int(os.path.basename(f)[4:-4]) < hi]
    return max(c, key=os.path.getmtime) if c else None


for sid, cells in sorted(stairs.items()):
    if only and sid not in only:
        continue
    goal = None
    # a horizontal pair (same row) is entered from above/below, a vertical pair from the left/right
    horiz = len(cells) > 1 and cells[0][1] == cells[1][1]
    dirs = ((0, 1, '-row'), (0, -1, '+row'), (1, 0, '-col'), (-1, 0, '+col')) if horiz else            ((1, 0, '-col'), (-1, 0, '+col'), (0, 1, '-row'), (0, -1, '+row'))
    for (c, r) in cells:
        for dc, dr, push in dirs:
            n = (c + dc, r + dr)
            if not solid(*n):
                goal, direction = n, push
                break
        if goal:
            break
    if not goal:
        print(f'{sid:04x} {cells}: no free neighbour'); continue
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'cellwalk.py'), chunk, str(area), str(src), f'{goal[0]},{goal[1]}', str(out)],
                   cwd=ROOT, capture_output=True)
    f = latest(out, out + 50)
    s = os.path.basename(f)[4:-4] if f else src
    cell, fl = st(s)
    if cell != goal:
        print(f'{sid:04x} {cells}: could not reach {goal} (at {cell})', flush=True)
        out += 60
        continue
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'stairpush.py'), s, str(out + 55), direction], cwd=ROOT, capture_output=True)
    cell2, fl2 = st(out + 55)
    print(f'{sid:04x} {cells}: from {goal} -> floor {fl2} cell {cell2} (slot {out + 55})', flush=True)
    out += 60
