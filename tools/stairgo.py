"""Walk to the cell in front of a stairs/exit (tools/diagwalk.py) and push each stick direction until the floor byte or the
area changes (or the party enters a room scene).

usage: stairgo.py <chunk0.bin> <area> <from-slot> <col,row> <first-out-slot> [frames-per-step=12]
Prints 'new <slot> <where>' on success."""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
chunk, area, src, goal, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], int(sys.argv[5])
fps = sys.argv[6] if len(sys.argv) > 6 else '12'
env = dict(os.environ, P2_EXE=os.environ.get('P2_EXE', os.path.join(ROOT, 'build', 'dev', 'p2.exe')), P2_NO_ENCOUNTERS='1')


def where(s):
    return subprocess.run([sys.executable, 'tools/where.py', str(s)], capture_output=True, text=True, cwd=ROOT).stdout.strip()


def floor_cell(s):
    with open(os.path.join(ROOT, 'states', f'slot{s}.p2s'), 'rb') as f:
        f.seek(24); R = f.read(0x2000000)
    return R[0xce819d], (R[0xceb2e0], R[0xceb2e4])


r = subprocess.run([sys.executable, 'tools/diagwalk.py', chunk, area, str(src), goal, str(out), fps], capture_output=True, text=True,
                   cwd=ROOT, env=env).stdout.strip().splitlines()
s = src
for line in r[::-1]:
    if line.startswith('final slot'): s = int(line.split()[2]); break
fl, c = floor_cell(s)
print('at', c, 'floor', fl, flush=True)
for d in ('UP', 'DOWN', 'LEFT', 'RIGHT', 'UP+LEFT', 'UP+RIGHT', 'DOWN+LEFT', 'DOWN+RIGHT'):
    t = out + 90
    subprocess.run([sys.executable, 'tools/drive.py', str(s), str(t), '400', f'5={d}:200'], cwd=ROOT, env=env, capture_output=True)
    f2, c2 = floor_cell(t)
    far = abs(c2[0] - c[0]) + abs(c2[1] - c[1]) > 6
    if f2 != fl or c2 == (0, 0):
        print('new', t, d, where(t)); sys.exit(0)
print('no exit found from', s)
