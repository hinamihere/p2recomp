"""Enter a door from the cell next to it: try each stick direction (face it, then Cross) until the dungeon cell bytes
become (0,0) (a room scene opened) or the floor byte changes; saves that state.

usage: doorpush.py <from-slot> <to-slot> [first-direction]"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get('P2_EXE', os.path.join(ROOT, 'build', 'dev', 'p2.exe'))
src, dst = sys.argv[1], sys.argv[2]
order = ['UP', 'DOWN', 'LEFT', 'RIGHT']
if len(sys.argv) > 3:
    order.remove(sys.argv[3]); order.insert(0, sys.argv[3])
env = dict(os.environ, P2_EXE=EXE, P2_NO_ENCOUNTERS='1')
for k in ('P2_GOTO', 'P2_GOTO_INT', 'P2_GOTO_TOL', 'P2_GOTO_AXES'):
    env.pop(k, None)


def st(s):
    with open(os.path.join(ROOT, 'states', f'slot{s}.p2s'), 'rb') as f:
        f.seek(24)
        r = f.read(0x2000000)
    return (r[0xCEB2E0], r[0xCEB2E4]), r[0xCE819D]


c0, f0 = st(src)
for d in order:
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'drive.py'), src, dst, '500', f'10={d}:6,60=CROSS:4,200=CROSS:4'],
                   env=env, cwd=ROOT, capture_output=True)
    c, f = st(dst)
    print(d, c, 'floor', f)
    if c == (0, 0) or f != f0:
        print('entered with', d)
        break
