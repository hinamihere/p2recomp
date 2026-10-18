"""Steer the party along a dungeon path with long stick holds (much faster than tools/diagwalk.py's per-step runs).

usage: steer.py <chunk> <area> <from-slot> <out-slot> <goal c,r> [passable-hex-nibbles, default e,1,3,4]
Plans a 4-way BFS path over plain floor (+ the given tile classes, by high nibble), calibrates which stick direction moves the
cell by +-1 in each axis, then for each straight segment holds that direction ~11 vblanks per cell and re-plans from wherever the
party ended up. Stops at the goal, when the party enters a room (cell 0,0) or after 25 holds. Prints the final slot."""
import collections
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
chunk, area, src, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
goal = tuple(int(v) for v in sys.argv[5].split(','))
extra = {int(c, 16) for c in (sys.argv[6] if len(sys.argv) > 6 else 'e,1,4').split(',')}
env = dict(os.environ, P2_NO_ENCOUNTERS='1')

d = open(os.path.join(ROOT, chunk), 'rb').read()
o = 0x34 + 40 * area
w, h = struct.unpack_from('<HH', d, o)
offs = struct.unpack_from('<7I', d, o + 12)
W, H = 2 * w, 2 * h
B = [struct.unpack_from('<H', d, offs[1] + 2 * i)[0] for i in range(W * H)]
FLOW = {0: (0, 1), 1: (-1, 0), 2: (0, -1), 3: (1, 0)}  # sand escalators 0x340n (n=1 west and n=2 north seen; 0/3 guessed)


def ok(c, v=None):
    if c == goal:
        return True
    if not (0 <= c[0] < W and 0 <= c[1] < H):
        return False
    x = B[c[1] * W + c[0]]
    if x == 0 or x >> 12 in extra or x == 0x2001:
        return True
    return x >> 12 == 3 and 3 not in extra and v == FLOW[x & 3]  # ride an escalator only along its flow


def cell(s):
    with open(os.path.join(ROOT, 'states', f'slot{s}.p2s'), 'rb') as f:
        f.seek(24)
        R = f.read(0x2000000)
    return R[0xCEB2E0], R[0xCEB2E4]


def drive(s, o, frames, inp):
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'drive.py'), str(s), str(o), str(frames), inp],
                   env=env, cwd=ROOT, capture_output=True)


def path(a):
    prev = {a: None}
    q = collections.deque([a])
    while q:
        c = q.popleft()
        if c == goal:
            break
        for i, j in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            n = (c[0] + i, c[1] + j)
            if n not in prev and ok(n, (i, j)):
                prev[n] = c
                q.append(n)
    if goal not in prev:
        return None
    p, c = [], goal
    while c:
        p.append(c)
        c = prev[c]
    return p[::-1]


# calibrate: which stick direction moves the cell in which way
start = cell(src)
dirs = {}
for k in ('UP', 'DOWN', 'LEFT', 'RIGHT'):
    drive(src, 5698, 50, f'5={k}:30')
    c = cell(5698)
    dx, dy = c[0] - start[0], c[1] - start[1]
    if abs(dx) > abs(dy) and dx:
        dirs[(1 if dx > 0 else -1, 0)] = k
    elif dy:
        dirs[(0, 1 if dy > 0 else -1)] = k
opp = {'UP': 'DOWN', 'DOWN': 'UP', 'LEFT': 'RIGHT', 'RIGHT': 'LEFT'}
for (vx, vy), k in list(dirs.items()):
    dirs.setdefault((-vx, -vy), opp[k])
print('calibration', dirs, flush=True)

s, n = src, out
for step in range(25):
    c = cell(s)
    if c == (0, 0):
        print('entered a room'); break
    if c == goal:
        break
    p = path(c)
    if not p or len(p) < 2:
        print('no path from', c); break
    v = (p[1][0] - p[0][0], p[1][1] - p[0][1])
    run = 1
    while run + 1 < len(p) and (p[run + 1][0] - p[run][0], p[run + 1][1] - p[run][1]) == v:
        run += 1
    k = dirs.get(v)
    if not k:
        print('no stick direction for', v); break
    hold = max(8, 11 * run)
    drive(s, n, hold + 20, f'5={k}:{hold}')
    print(f'{c} {k}x{run} -> {cell(n)}', flush=True)
    s, n = n, n + 1
print('final', s, cell(s))
