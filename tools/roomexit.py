"""Leave a room scene: walk to a room point (int room coords at 08e44c14/1c), then try each of the 8 stick
directions with Cross until the dungeon cell (08ceb2e0/e4) becomes non-zero, i.e. the party is back on the floor map.

usage: roomexit.py <from-slot> <to-slot> <x> <z> [ux,uz,rx,rz]"""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get('P2_EXE', os.path.join(ROOT, 'build', 'dev', 'p2.exe'))
src, dst, x, z = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
axes = sys.argv[5] if len(sys.argv) > 5 else None


def state(s):
    with open(os.path.join(ROOT, 'states', f'slot{s}.p2s'), 'rb') as f:
        f.seek(24)
        r = f.read(0x2000000)
    return (r[0xCEB2E0], r[0xCEB2E4]), struct.unpack_from('<3i', r, 0xE44C14)


def drive(a, b, n, inp, extra=None):
    env = dict(os.environ, P2_EXE=EXE, P2_NO_ENCOUNTERS='1', **(extra or {}))
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'drive.py'), str(a), str(b), str(n), inp], env=env, cwd=ROOT,
                   capture_output=True)


start = src
if axes:
    drive(src, 5798, 1500, '', {'P2_GOTO_INT': '1', 'P2_GOTO_AXES': axes, 'P2_GOTO': f'8e44c14,8e44c18,8e44c1c;{x},{z}'})
    start = 5798
print('at', state(start))
for d in ('UP', 'DOWN', 'LEFT', 'RIGHT', 'UP+LEFT', 'UP+RIGHT', 'DOWN+LEFT', 'DOWN+RIGHT'):
    drive(start, dst, 500, f'5={d}:12,40=CROSS:4,120=CROSS:4')
    cell, room = state(dst)
    print(d, cell, room)
    if cell != (0, 0):
        print('left the room with', d)
        break
