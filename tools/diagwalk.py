"""8-way cell walker for dungeons with diagonal cave walls (Mt. Iwato F0110): plans on layer B (0x8000 = solid,
0xE0nn = diagonal wall half-cells, walkable (blocked moves are learned); 0x24nn warps solid unless they are the goal) with diagonal steps,
and drives one stick push per step (UP = row-1, RIGHT = col+1 unless P2_DIAG_AXES=ux,uy,rx,ry says otherwise).

usage: diagwalk.py <chunk0.bin> <area> <from-slot> <col,row> <first-out-slot> [frames-per-step=12]
Each step is saved to the next slot; prints the final slot."""
import collections
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
d = open(sys.argv[1], 'rb').read()
area = int(sys.argv[2]); slot = int(sys.argv[3]); goal = tuple(map(int, sys.argv[4].split(','))); out = int(sys.argv[5])
fps = int(sys.argv[6]) if len(sys.argv) > 6 else 12
o = 0x34 + 40 * area; w, h = struct.unpack_from('<HH', d, o); offs = struct.unpack_from('<7I', d, o + 12)
W, H = 2 * w, 2 * h
B = [struct.unpack_from('<H', d, offs[1] + 2 * i)[0] for i in range(W * H)]
C = [struct.unpack_from('<H', d, offs[2] + 2 * i)[0] & 0xff for i in range(W * H)]
ux, uy, rx, ry = map(int, os.environ.get('P2_DIAG_AXES', '0,-1,1,0').split(','))
env = dict(os.environ, P2_EXE=os.environ.get('P2_EXE', os.path.join(ROOT, 'build', 'dev', 'p2.exe')), P2_NO_ENCOUNTERS='1')


# P2_DIAG_PITMARGIN=1: also keep one cell away from pits (the walker overshoots by about a cell)
PIT = {(c, r) for r in range(H) for c in range(W) if B[r * W + c] == 0x0005}
NEAR = {(c + i, r + j) for c, r in PIT for i in (-1, 0, 1) for j in (-1, 0, 1)} if os.environ.get('P2_DIAG_PITMARGIN') == '1' else set()


def free(c, r, use_e=True):
    if not (0 <= c < W and 0 <= r < H): return False
    b = B[r * W + c]
    if (c, r) == goal: return True
    if not use_e and b >> 12 == 0xE: return False
    if (c, r) in NEAR: return False
    return b not in (0x8000, 0x0005) and b >> 8 != 0x24 and C[r * W + c] not in (0x3b, 0x3d)  # 0x0005 = pit (B2F)


def cell(s):
    f = open(os.path.join(ROOT, 'states', f'slot{s}.p2s'), 'rb'); f.seek(24); R = f.read(0x2000000)
    return (R[0xceb2e0], R[0xceb2e4]), R[0xce819d]


blocked = set()


def plan(a):
    return plan1(a, False) or plan1(a, True)  # plain floor first, the diagonal half-cells only when needed


def plan1(a, use_e):
    prev = {a: None}; q = collections.deque([a])
    while q:
        c = q.popleft()
        if c == goal: break
        for dc in (-1, 0, 1):
            for dr in (-1, 0, 1):
                if dc == dr == 0: continue
                n = (c[0] + dc, c[1] + dr)
                if n in prev or not free(*n, use_e) or (c, n) in blocked: continue
                prev[n] = c; q.append(n)
    if goal not in prev: return None
    p = []; c = goal
    while c: p.append(c); c = prev[c]
    return p[::-1]


# stick name for a (dc, dr) step: solve dc = u*ux + r*rx, dr = u*uy + r*ry with u, r in -1..1
def make_names():
    nm = {}
    for u in (-1, 0, 1):
        for r in (-1, 0, 1):
            if u == r == 0: continue
            dc, dr = u * ux + r * rx, u * uy + r * ry
            nm[(dc, dr)] = '+'.join(x for x in (('UP' if u > 0 else 'DOWN') if u else '', ('RIGHT' if r > 0 else 'LEFT') if r else '') if x)
    return nm


def calib(s):
    """the camera turns between cave zones: probe UP / RIGHT (DOWN / LEFT when blocked) and keep the dominant axis"""
    c0, _ = cell(s); m = {}
    for dn in ('UP', 'RIGHT', 'DOWN', 'LEFT'):
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'drive.py'), str(s), '5797', '70', f'5={dn}:40'],
                       env=env, cwd=ROOT, capture_output=True)
        c1, _ = cell(5797); m[dn] = (c1[0] - c0[0], c1[1] - c0[1])
    sg = lambda v: (v > 0) - (v < 0)
    up = m['UP'] if m['UP'] != (0, 0) else tuple(-v for v in m['DOWN'])
    rt = m['RIGHT'] if m['RIGHT'] != (0, 0) else tuple(-v for v in m['LEFT'])
    if up == (0, 0) and rt != (0, 0): up = (rt[1], -rt[0]) if abs(rt[0]) >= abs(rt[1]) else (rt[1], -rt[0])
    if rt == (0, 0) and up != (0, 0): rt = (-up[1], up[0])
    up = (sg(up[0]), 0) if abs(up[0]) >= abs(up[1]) else (0, sg(up[1]))
    rt = (sg(rt[0]), 0) if abs(rt[0]) >= abs(rt[1]) else (0, sg(rt[1]))
    if up[0] * rt[1] - up[1] * rt[0] == 0: return (ux, uy, rx, ry)  # degenerate probe (pushed into a corner): keep the old axes
    return up + rt


names = make_names()
for it in range(400):
    cur, fl = cell(slot)
    if cur == goal: break
    p = plan(cur)
    if not p: print('no route from', cur); break
    step = (p[1][0] - cur[0], p[1][1] - cur[1])
    if step not in names: step = (step[0], 0) if (step[0], 0) in names else (0, step[1])
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'drive.py'), str(slot), str(out), str(fps + 20),
                    f'5={names[step]}:{fps}'], env=env, cwd=ROOT, capture_output=True)
    new, nf = cell(out)
    if nf != fl: print('floor change', fl, '->', nf, 'at', new); slot = out; break
    if new == cur:
        a2 = calib(slot)
        if a2 != (ux, uy, rx, ry) and a2[:2] != (0, 0) and a2[2:] != (0, 0):
            ux, uy, rx, ry = a2; names = make_names(); print('recalibrated', a2); continue
        blocked.add((cur, p[1])); print('blocked', cur, '->', p[1], names[step]); continue
    if new != p[1]:
        a2 = calib(slot)  # moved somewhere else: the camera turned; probe from the old spot
        if (new in NEAR or new in PIT) or a2 != (ux, uy, rx, ry):
            if a2 != (ux, uy, rx, ry) and a2[:2] != (0, 0) and a2[2:] != (0, 0):
                ux, uy, rx, ry = a2; names = make_names(); print('recalibrated after drift', a2)
            print('rejected step', cur, '->', new); continue  # keep the old slot
    slot, out = out, out + 1
print('final slot', slot, cell(slot))
