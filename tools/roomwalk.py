"""Greedy room walker with obstacle sliding: steer the leader (int32 room position at 08e44c14/1c) toward a point,
and when a step makes no progress try the perpendicular stick directions (sliding along beams, desks and party members).

usage: roomwalk.py <from-slot> <to-slot> <x> <z> <ux,uz,rx,rz> [tolerance=40] [max-steps=80]
The axes are the room (dx,dz) of stick UP and RIGHT (see the calibration probes in README); the 8 stick directions are
scored by their dot product with the wanted direction. Every step is one 10-frame hold saved to <to-slot>."""
import math
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get('P2_EXE', os.path.join(ROOT, 'build', 'dev', 'p2.exe'))
src, dst, gx, gz = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
ux, uz, rx, rz = (float(v) for v in sys.argv[5].split(','))
tol = float(sys.argv[6]) if len(sys.argv) > 6 else 40
maxsteps = int(sys.argv[7]) if len(sys.argv) > 7 else 80
env = dict(os.environ, P2_EXE=EXE, P2_NO_ENCOUNTERS='1')
for k in ('P2_GOTO', 'P2_GOTO_INT', 'P2_GOTO_TOL', 'P2_GOTO_AXES'):
    env.pop(k, None)
DIRS = {'UP': (ux, uz), 'DOWN': (-ux, -uz), 'RIGHT': (rx, rz), 'LEFT': (-rx, -rz),
        'UP+RIGHT': (ux + rx, uz + rz), 'UP+LEFT': (ux - rx, uz - rz), 'DOWN+RIGHT': (-ux + rx, -uz + rz), 'DOWN+LEFT': (-ux - rx, -uz - rz)}


def pos(s):
    with open(os.path.join(ROOT, 'states', f'slot{s}.p2s'), 'rb') as f:
        f.seek(24)
        r = f.read(0x2000000)
    x, _, z = struct.unpack_from('<3i', r, 0xE44C14)
    return x, z, (r[0xCEB2E0], r[0xCEB2E4])


def step(s, d, frames=10):
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'drive.py'), str(s), str(dst), str(frames + 20), f'5={d}:{frames}'],
                   env=env, cwd=ROOT, capture_output=True)
    return pos(dst)


cur = src
blocked = {}
for it in range(maxsteps):
    x, z, cell = pos(cur)
    dx, dz = gx - x, gz - z
    dist = math.hypot(dx, dz)
    print(f'at ({x},{z}) dist {dist:.0f}', flush=True)
    if dist < tol or cell != (0, 0):
        break
    ranked = sorted(DIRS, key=lambda d: -(DIRS[d][0] * dx + DIRS[d][1] * dz) / math.hypot(*DIRS[d]) + 1e9 * blocked.get(d, 0))
    moved = False
    for d in ranked[:5]:
        nx, nz, _ = step(cur, d)
        if math.hypot(nx - x, nz - z) > 15:
            cur = dst
            # copy so the next step can read it while dst is rewritten
            subprocess.run(['cp', os.path.join(ROOT, 'states', f'slot{dst}.p2s'), os.path.join(ROOT, 'states', f'slot{int(dst) + 1}.p2s')])
            cur = str(int(dst) + 1)
            moved = True
            blocked = {k: v - 1 for k, v in blocked.items() if v > 1}
            break
        blocked[d] = 3
    if not moved:
        print('stuck'); break
print('final', pos(cur), 'slot', cur)
