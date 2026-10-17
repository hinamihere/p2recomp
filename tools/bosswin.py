"""Win a battle with the HP cheat: find the battle party table in a battle savestate (6+ consecutive 176-byte records whose
first words are hp / hp / max hp / sp), poke 999 HP for the party and 1 HP for every enemy record after it, then press
Triangle (Auto) + Cross until the battle ends; Level Up screens get RIGHT x9 + Cross per character.

usage: bosswin.py <battle-slot> <to-slot> [party-size]
Finds the table by scanning 0x09000000-0x09C00000 for party records (hp <= max hp <= 999, sp field 1..999) followed by
records with larger HP (enemies); the second copy 0xB00 bytes later is patched too."""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get('P2_EXE', os.path.join(ROOT, 'build', 'dev', 'p2.exe'))
src, dst = sys.argv[1], sys.argv[2]
n_party = int(sys.argv[3]) if len(sys.argv) > 3 else 0
with open(os.path.join(ROOT, 'states', f'slot{src}.p2s'), 'rb') as f:
    head = f.read(24)
    R = f.read(0x2000000)
vblank = struct.unpack('<IIIQ', head[:20])[3]


def rec(off):
    return struct.unpack_from('<4H', R, off)


best = None
# the battle table lives at 0x094AB290 (copy +0xB00) in every fight seen so far: party records (hp, hp, max hp, sp) then
# enemy records with sp 0
# (0x094EB2A0 in the King Leo fight on the airship)
for KNOWN in (0x094AB290 - 0x08000000, 0x094EB2A0 - 0x08000000):
    k = 0
    while k < 6 and 0 < rec(KNOWN + 176 * k)[0] <= rec(KNOWN + 176 * k)[2] <= 999 and rec(KNOWN + 176 * k)[3] > 0:
        k += 1
    if k and rec(KNOWN + 176 * k)[0] > 0 and rec(KNOWN + 176 * k)[3] == 0:
        best = (KNOWN, k)
        break
layout = 'A'
if not best:
    B = 0x0950BAA2 - 0x08000000
    kb = 0
    while kb < 6 and 0 < rec(B + 176 * kb)[1] <= 999 and rec(B + 176 * kb)[2] > 0:
        kb += 1
    if kb and rec(B + 176 * kb)[0] > 0 and rec(B + 176 * kb)[2] == 0:
        best, layout = (B, kb), 'B'
for off in ([] if best else range(0x1000000, 0x1C00000, 4)):
    hp, hp2, mhp, sp = rec(off)
    if not (0 < hp <= mhp <= 999 and hp == hp2 and 0 < sp <= 999):
        continue
    k = 0
    while k < 6:
        a, b, c, d = rec(off + 176 * k)
        if not (0 < a <= c <= 999 and a == b and 0 < d <= 999):
            break
        k += 1
    if k >= 2 and (n_party == 0 or k == n_party):
        e = rec(off + 176 * k)
        if e[0] > 0 and e[3] == 0:
            best = (off, k)
            break
# layouts: A = hp, hp, max hp, sp (table at 0x094AB290); B = hp, max hp, sp, max sp (table at 0x0950BAA2)
if not best:
    sys.exit('party table not found')
off, k = best
print(f'party table at {0x08000000 + off:08x} (layout {layout}), {k} members, enemy hp {rec(off + 176 * k)[0]}')
pokes = []
for base in (off, off + 0xB00):
    for i in range(k):
        if layout == 'A':
            pokes.append(f'{0x08000000 + base + 176 * i:x},x03e703e7')
            pokes.append(f'{0x08000000 + base + 176 * i + 4:x},x{rec(off + 176 * i)[3]:04x}03e7')
        else:
            pokes.append(f'{0x08000000 + base + 176 * i:x},x03e703e7')
    for j in range(k, 8):
        if rec(off + 176 * j)[0] > 0:
            pokes.append(f'{0x08000000 + base + 176 * j:x},x00010001')
env = dict(os.environ, P2_EXE=EXE, P2_NO_ENCOUNTERS='1', P2_POKE=f'{vblank + 1},' + ','.join(pokes))
inp = [f'{v}=TRIANGLE:4,{v + 30}=CROSS:4' for v in range(10, 1800, 60)]
v = 1850
for c in range(6):
    for _ in range(9):
        inp.append(f'{v}=RIGHT:3'); v += 8
    inp.append(f'{v}=CROSS:4'); v += 90
    inp.append(f'{v}=CROSS:4'); v += 90
inp += [f'{w}=CROSS:4' for w in range(v, v + 1500, 40)]
subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'drive.py'), src, dst, str(v + 1600), ','.join(inp), '--sheet', '12'],
               env=env, cwd=ROOT, capture_output=True)
print('done ->', dst)
