"""Leave a shop room (at its exit mat) back to the town map: try each stick direction with Cross (opens the shopping-mall
menu) and Circle (back to the town map) until the town-map node table (pointer at 0x08E4435C) holds a valid exit node,
detected as the node-table pointer at 0x08E4435C changing (the town map reloads its graph).

usage: mallexit.py <from-slot> <to-slot>"""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get('P2_EXE', os.path.join(ROOT, 'build', 'dev', 'p2.exe'))
src, dst = sys.argv[1], sys.argv[2]
env = dict(os.environ, P2_EXE=EXE, P2_NO_ENCOUNTERS='1')
for k in ('P2_GOTO', 'P2_GOTO_INT', 'P2_GOTO_TOL', 'P2_GOTO_AXES'):
    env.pop(k, None)


def room(s):
    with open(os.path.join(ROOT, 'states', f'slot{s}.p2s'), 'rb') as f:
        f.seek(24)
        r = f.read(0x2000000)
    return struct.unpack_from('<3i', r, 0xE44C14), r


r0, R0 = room(src)
for d in ('DOWN+RIGHT', 'DOWN', 'RIGHT', 'DOWN+LEFT', 'LEFT', 'UP+RIGHT', 'UP', 'UP+LEFT'):
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'drive.py'), src, dst, '700', f'5={d}:8,40=CROSS:4,400=CIRCLE:4'],
                   env=env, cwd=ROOT, capture_output=True)
    r1, R1 = room(dst)
    tab0 = struct.unpack_from('<I', R0, 0xE4435C)[0]
    tab1 = struct.unpack_from('<I', R1, 0xE4435C)[0]
    print(d, r1, 'map reloaded' if tab1 != tab0 else '')
    if tab1 != tab0:
        print('left with', d)
        break
