"""Close open dialogue until the leader can move: alternately press Cross (or Circle) and test a short stick push; stops as soon
as the room position (08e44c14/1c) or dungeon cell (08ceb2e0/e4) changes, and saves the state *before* the test push.

usage: settle.py <from-slot> <to-slot> [button=CROSS] [tries=20] [test-dir=UP]"""
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get('P2_EXE', os.path.join(ROOT, 'build', 'dev', 'p2.exe'))
src, dst = sys.argv[1], sys.argv[2]
btn = sys.argv[3] if len(sys.argv) > 3 else 'CROSS'
tries = int(sys.argv[4]) if len(sys.argv) > 4 else 20
tdir = sys.argv[5] if len(sys.argv) > 5 else 'UP'
env = dict(os.environ, P2_EXE=EXE, P2_NO_ENCOUNTERS='1')
for k in ('P2_GOTO', 'P2_GOTO_INT', 'P2_GOTO_TOL', 'P2_GOTO_AXES'):
    env.pop(k, None)
SCRATCH = 5796


def st(s):
    with open(os.path.join(ROOT, 'states', f'slot{s}.p2s'), 'rb') as f:
        f.seek(24)
        r = f.read(0x2000000)
    return struct.unpack_from('<3i', r, 0xE44C14), (r[0xCEB2E0], r[0xCEB2E4]), r[0xCE819D]


def drive(a, b, n, inp):
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'drive.py'), str(a), str(b), str(n), inp], env=env, cwd=ROOT,
                   capture_output=True)


def path(s):
    return os.path.join(ROOT, 'states', f'slot{s}.p2s')


cur = src
for i in range(tries):
    before = st(cur)
    drive(cur, SCRATCH, 40, f'5={tdir}:12')
    if st(SCRATCH) != before:
        if str(cur) != str(dst):
            shutil.copy(path(cur), path(dst))
        print('free after', i, 'presses:', before)
        sys.exit(0)
    drive(cur, dst, 50, f'5={btn}:4')
    cur = dst
print('still busy after', tries, 'presses')
