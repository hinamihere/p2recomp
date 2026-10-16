"""Talk to a rumormonger and pick "Talk with ..." (the 2nd entry of the Ask / Talk / Never mind menu).

usage: rumortalk.py <slot-next-to-npc> <to-slot> <facing-direction>
The intro text has a varying number of pages, so the script tries n = 4..14 Cross presses before DOWN + Cross, then pages
through the answer, and keeps the first try whose story-flag array (0x08E3B414, 0x200 bytes) gained new bits other than the
ones the first page alone sets. Prints the new flag numbers."""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get('P2_EXE', os.path.join(ROOT, 'build', 'dev', 'p2.exe'))
src, dst, facing = sys.argv[1], sys.argv[2], sys.argv[3]
env = dict(os.environ, P2_EXE=EXE, P2_NO_ENCOUNTERS='1')
for k in ('P2_GOTO', 'P2_GOTO_INT', 'P2_GOTO_TOL', 'P2_GOTO_AXES'):
    env.pop(k, None)


def flags(s):
    with open(os.path.join(ROOT, 'states', f'slot{s}.p2s'), 'rb') as f:
        f.seek(24 + 0xE3B414)
        b = f.read(0x200)
    return {i * 8 + j for i, v in enumerate(b) for j in range(8) if v >> j & 1}


def drive(a, b, n, inp):
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'drive.py'), str(a), str(b), str(n), inp], env=env, cwd=ROOT,
                   capture_output=True)


base = flags(src)
# reference: the same number of plain Cross presses (intro and Ask-path flags), so only the Talk answer's flags remain
for n in range(4, 15):
    v = 40
    inp = [f'5={facing}:2', f'{v}=CROSS:4']
    for _ in range(n):
        v += 60
        inp.append(f'{v}=CROSS:4')
    ref = inp + [f'{v + 60 + 60 * k}=CROSS:4' for k in range(12)]
    talk = inp + [f'{v + 50}=DOWN:3', f'{v + 80}=CROSS:4'] + [f'{v + 140 + 60 * k}=CROSS:4' for k in range(12)]
    drive(src, 5795, v + 900, ','.join(ref))
    drive(src, dst, v + 900, ','.join(talk))
    new = flags(dst) - base - (flags(5795) - base)
    print(n, 'new flags', sorted(new), flush=True)
    if new:
        break
