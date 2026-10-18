"""Win a battle with reinforcement waves: in chunks of N vblanks, hold the party at 999 HP and set every live enemy record to
1 HP at the start of each chunk, pressing Triangle (Auto) + Cross, until the battle table is gone (or max chunks).

usage: wavewin.py <battle-slot> <first-out-slot> <table-hex> [party=5] [chunk=400] [max=40]
The table is layout A (hp, hp, max hp, sp; copy +0xB00), e.g. 0955f4a0 for the Mt. Katatsumuri soldiers."""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src, out, table = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3], 16)
party = int(sys.argv[4]) if len(sys.argv) > 4 else 5
chunk = int(sys.argv[5]) if len(sys.argv) > 5 else 400
maxc = int(sys.argv[6]) if len(sys.argv) > 6 else 40
env = dict(os.environ, P2_EXE=os.environ.get('P2_EXE', os.path.join(ROOT, 'build', 'dev', 'p2.exe')), P2_NO_ENCOUNTERS='1',
           P2_POKE_HOLD='1')


def load(s):
    with open(os.path.join(ROOT, 'states', f'slot{s}.p2s'), 'rb') as f:
        h = f.read(24); R = f.read(0x2000000)
    return struct.unpack('<IIIQ', h[:20])[3], R


s = src
for c in range(maxc):
    v, R = load(s)
    rec = lambda i: struct.unpack_from('<4H', R, table - 0x08000000 + 176 * i)
    if not (0 < rec(0)[2] <= 999 and rec(0)[3] > 0):
        print('battle over at', s); break
    alive = [i for i in range(party, party + 12) if rec(i)[0] > 1 and rec(i)[3] == 0]
    pokes = [f'{table + o + 176 * i:x},x03e703e7' for o in (0, 0xB00) for i in range(party)]
    # enemies: a one-shot poke needs a separate run; fold it into the first frames by holding only for 3 vblanks is not
    # possible with P2_POKE_HOLD, so write 1 HP every frame for this chunk - dead enemies (hp 0) are left alone
    pokes += [f'{table + o + 176 * i:x},x00010001' for o in (0, 0xB00) for i in alive]
    pokes += [x for x in os.environ.get('WAVE_EXTRA_POKES', '').split(',') if x]  # e.g. persistent party HP
    inp = ','.join(f'{t}=TRIANGLE:4,{t + 30}=CROSS:4' for t in range(10, chunk, 80))
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'drive.py'), str(s), str(out), str(chunk), inp],
                   env=dict(env, P2_POKE=f'{v + 1},' + ','.join(pokes)), cwd=ROOT, capture_output=True)
    print('chunk', c, 'alive', alive, '->', out, flush=True)
    s, out = out, out + 1
print('final', s)
