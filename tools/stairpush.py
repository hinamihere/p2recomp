"""Push the party onto stairs or into a door cell: calibrate the stick at the slot, then hold the direction that moves along +row/-row/+col/-col
in growing steps until the floor byte (0x08CE819D) changes or the cell bytes become (0,0) (a room scene opened); saves that state.

usage: stairpush.py <from-slot> <to-slot> <+row|-row|+col|-col>"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get('P2_EXE', os.path.join(ROOT, 'build', 'dev', 'p2.exe'))
src, dst, want = sys.argv[1], sys.argv[2], sys.argv[3]
env = dict(os.environ, P2_EXE=EXE, P2_NO_ENCOUNTERS='1')
for k in ('P2_GOTO', 'P2_GOTO_INT', 'P2_GOTO_TOL', 'P2_GOTO_AXES'):
    env.pop(k, None)


def st(s):
    with open(os.path.join(ROOT, 'states', f'slot{s}.p2s'), 'rb') as f:
        f.seek(24)
        r = f.read(0x2000000)
    return r[0xCEB2E0], r[0xCEB2E4], r[0xCE819D]


def drive(a, b, n, inp):
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'drive.py'), str(a), str(b), str(n), inp], env=env, cwd=ROOT,
                   capture_output=True)


c0, r0, f0 = st(src)
axis, sign = (1, 1) if want == '+row' else (1, -1) if want == '-row' else (0, 1) if want == '+col' else (0, -1)
best = None
for d in ('UP', 'DOWN', 'LEFT', 'RIGHT'):
    drive(src, 5797, 90, f'10={d}:12')
    c, r, f = st(5797)
    if f != f0:
        best = d
        break
    delta = (c - c0, r - r0)[axis] * sign
    if delta > 0:
        best = d
        break
if best is None:  # at the stair edge no step along the axis is possible without taking the stairs: try longer pushes
    for d in ('UP', 'DOWN', 'LEFT', 'RIGHT'):
        drive(src, 5797, 200, f'10={d}:40')
        if st(5797)[2] != f0 or st(5797)[:2] == (0, 0):
            best = d
            break
print('direction', best)
for n in (8, 16, 24, 32, 45, 60, 80):
    drive(src, dst, n + 150, f'5={best}:{n}')
    c, r, f = st(dst)
    print(n, (c, r), 'floor', f)
    if f != f0 or (c, r) == (0, 0):
        break
