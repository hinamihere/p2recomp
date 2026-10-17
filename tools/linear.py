"""Climb a linear multi-area dungeon: on each area take the stairs group (layer-B 0x28nn / 0x24nn markers, grouped by adjacency)
farthest from the arrival cell, via tools/stairgo.py, until a room scene starts or max steps.

usage: linear.py <chunk0.bin> <from-slot> <first-out-slot> [max-areas=15] [frames-per-step=12]"""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
chunk, s, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
maxa = int(sys.argv[4]) if len(sys.argv) > 4 else 15
fps = sys.argv[5] if len(sys.argv) > 5 else '12'
d = open(chunk, 'rb').read()


def area_info(a):
    o = 0x34 + 40 * a; w, h = struct.unpack_from('<HH', d, o); offs = struct.unpack_from('<7I', d, o + 12)
    W, H = 2 * w, 2 * h
    B = [struct.unpack_from('<H', d, offs[1] + 2 * i)[0] for i in range(W * H)]
    return W, H, B


def groups(a):
    W, H, B = area_info(a)
    cells = [(c, r) for r in range(H) for c in range(W) if B[r * W + c] >> 8 in (0x28, 0x24)]
    gs = []
    for c in cells:
        for g in gs:
            if any(abs(c[0] - x) + abs(c[1] - y) <= 1 for x, y in g): g.append(c); break
        else: gs.append([c])
    return W, H, B, gs


def state(n):
    with open(os.path.join(ROOT, 'states', f'slot{n}.p2s'), 'rb') as f:
        f.seek(24); R = f.read(0x2000000)
    return R[0xce819d], (R[0xceb2e0], R[0xceb2e4])


for step in range(maxa):
    a, cur = state(s)
    if cur == (0, 0): print('room scene at', s); break
    W, H, B, gs = groups(a)
    g = max(gs, key=lambda g: min(abs(x - cur[0]) + abs(y - cur[1]) for x, y in g))
    # front cell: a plain-floor neighbour of the group, closest to the arrival
    fronts = [(x + i, y + j) for x, y in g for i, j in ((0, 1), (0, -1), (1, 0), (-1, 0))
              if 0 <= x + i < W and 0 <= y + j < H and B[(y + j) * W + x + i] == 0]
    f = min(fronts, key=lambda p: abs(p[0] - cur[0]) + abs(p[1] - cur[1]))
    print(f'area {a} at {cur}: exit {g} via {f}', flush=True)
    # walk the plain-floor 8-way path in legs of 6 cells (diagwalk is unreliable over long distances)
    import collections
    prev = {cur: None}; q = collections.deque([cur])
    while q:
        c = q.popleft()
        if c == f: break
        for i in (-1, 0, 1):
            for j in (-1, 0, 1):
                n = (c[0] + i, c[1] + j)
                if n not in prev and 0 <= n[0] < W and 0 <= n[1] < H and B[n[1] * W + n[0]] == 0: prev[n] = c; q.append(n)
    if f in prev:
        path = []; c = f
        while c: path.append(c); c = prev[c]
        legs = path[::-1][6::6] + [f]
        for k, leg in enumerate(legs):
            try:
                subprocess.run([sys.executable, 'tools/diagwalk.py', chunk, str(a), str(s), f'{leg[0]},{leg[1]}', str(out + 1000 + 50 * k), fps],
                               capture_output=True, cwd=ROOT, timeout=600)
            except subprocess.TimeoutExpired: pass
            import glob
            cs = [int(x[11:-4]) for x in glob.glob('states/slot*.p2s') if x[11:-4].isdigit() and out + 1000 + 50 * k <= int(x[11:-4]) < out + 1050 + 50 * k]
            if cs: s = max(cs, key=lambda v: os.path.getmtime(f'states/slot{v}.p2s'))
        print('  walked to', state(s)[1], flush=True)
    r = subprocess.run([sys.executable, 'tools/stairgo.py', chunk, str(a), str(s), f'{f[0]},{f[1]}', str(out), fps],
                       capture_output=True, text=True, cwd=ROOT).stdout.strip().splitlines()
    print('  ', r[-1] if r else '?', flush=True)
    if not r or not r[-1].startswith('new'): break
    s = int(r[-1].split()[1]); out += 2000
    open(os.path.join(ROOT, 'states', 'KEEP'), 'a').write(str(s) + chr(10))
    subprocess.run([sys.executable, 'tools/prune_states.py', str(s)], cwd=ROOT, capture_output=True)
print('final', s)
