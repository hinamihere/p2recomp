"""Walk the party to a dungeon cell: plans on the area's walkability layer (layer C of a decoded F-file chunk 0, see
tools/p2lz.py), walks straight legs with the cell-based goto autopilot (P2_GOTO_INT + P2_GOTO_TOL=0.5 on the cell bytes
08ceb2e0/08ceb2e4), recalibrates the stick axes when a leg makes no progress (camera zones rotate them), learns blocked
steps the layer does not show (corner posts, NPCs) and replans.

usage: cellwalk.py <chunk0.bin> <area> <from-slot> <goal col,row> <first-out-slot>
Layer C: low byte 0x3b/0x3d solid, 1/2/3/4 = wall on the top/right/bottom/left side, 5-8 corner posts, else floor.
Writes one savestate per leg (from <first-out-slot> up; slot 5799 is scratch) and prints the final slot."""
import collections, os, struct, subprocess, sys
ROOT = os.getcwd()
chunk, area, slot, goal, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), tuple(map(int, sys.argv[4].split(','))), int(sys.argv[5])
d = open(chunk, 'rb').read()
o = 0x34 + 40 * area; w, h = struct.unpack_from('<HH', d, o); offs = struct.unpack_from('<7I', d, o + 12)
W, H = 2 * w, 2 * h
C = [struct.unpack_from('<H', d, offs[2] + 2 * i)[0] & 0xff for i in range(W * H)]
solid = lambda c, r: not (0 <= c < W and 0 <= r < H) or C[r * W + c] in (0x3b, 0x3d)
wall = lambda c, r, side: C[r * W + c] == side
moves = [((0, -1), 1, 3), ((1, 0), 2, 4), ((0, 1), 3, 1), ((-1, 0), 4, 2)]
blocked = set()
env0 = dict(os.environ, P2_EXE=os.environ.get('P2_EXE', os.path.join(ROOT, 'build', 'dev', 'p2.exe')), P2_NO_ENCOUNTERS='1')
for k in ('P2_GOTO', 'P2_GOTO_INT', 'P2_GOTO_TOL', 'P2_GOTO_AXES'): env0.pop(k, None)

def cell(s):
    f = open(f'states/slot{s}.p2s', 'rb'); f.seek(24); R = f.read(0x2000000)
    return (R[0xceb2e0], R[0xceb2e4]), R[0xce819d]

def drive(src, dst, n, inp, env):
    subprocess.run([sys.executable, 'tools/drive.py', str(src), str(dst), str(n), inp], env=env, cwd=ROOT, capture_output=True)

def calib(s):
    c0, _ = cell(s); m = {}
    for dname in ('UP', 'RIGHT', 'DOWN', 'LEFT'):
        drive(s, 5799, 90, f'10={dname}:30', env0)
        c1, _ = cell(5799); m[dname] = (c1[0] - c0[0], c1[1] - c0[1])
    sg = lambda v: (v > 0) - (v < 0)
    up = m['UP'] if m['UP'] != (0, 0) else tuple(-v for v in m['DOWN'])
    rt = m['RIGHT'] if m['RIGHT'] != (0, 0) else tuple(-v for v in m['LEFT'])
    # keep only the dominant component of each
    up = (sg(up[0]), 0) if abs(up[0]) >= abs(up[1]) else (0, sg(up[1]))
    rt = (sg(rt[0]), 0) if abs(rt[0]) >= abs(rt[1]) else (0, sg(rt[1]))
    return f'{up[0]},{up[1]},{rt[0]},{rt[1]}'

def plan(a):
    prev = {a: None}; q = collections.deque([a])
    while q:
        c = q.popleft()
        if c == goal: break
        for (dc, dr), s, opp in moves:
            n = (c[0] + dc, c[1] + dr)
            if n in prev or solid(*n) or wall(c[0], c[1], s) or wall(n[0], n[1], opp) or (c, n) in blocked: continue
            prev[n] = c; q.append(n)
    if goal not in prev: return None
    p = []; c = goal
    while c: p.append(c); c = prev[c]
    return p[::-1]

ax = None
for it in range(60):
    cur, fl = cell(slot)
    print('at', cur, 'floor', fl, flush=True)
    if cur == goal: break
    p = plan(cur)
    if not p: print('no route'); break
    # straight leg
    dirn = (p[1][0] - p[0][0], p[1][1] - p[0][1]); leg = [p[1]]
    for a, b in zip(p[1:], p[2:]):
        if (b[0] - a[0], b[1] - a[1]) != dirn: break
        leg.append(b)
    if ax is None: ax = calib(slot)
    env = dict(env0, P2_GOTO_INT='1', P2_GOTO_TOL='0.5', P2_GOTO_AXES=ax,
               P2_GOTO='8ceb2e0,8ceb2e0,8ceb2e4;' + ';'.join(f'{c},{r}' for c, r in leg))
    drive(slot, out, 120 + 60 * len(leg), '', env)
    new, _ = cell(out)
    if new == cur:
        ax2 = calib(slot)
        if ax2 != ax: ax = ax2; print('recalibrated', ax); continue
        blocked.add((cur, p[1])); print('blocked', cur, '->', p[1]); continue
    slot, out = out, out + 1
print('final slot', slot, cell(slot))
