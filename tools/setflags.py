"""Copy a savestate with story flags set or cleared (flag n = bit n%8 of byte n/8 at 0x08E3B414).

usage: setflags.py <from-slot> <to-slot> <flag>[,<flag>...] [--clear]
Runs the emulator from the slot, pokes the flag words one vblank in (P2_POKE) and saves the new slot a few vblanks later."""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get('P2_EXE', os.path.join(ROOT, 'build', 'dev', 'p2.exe'))
BASE = 0x08E3B414
src, dst, flags = sys.argv[1], sys.argv[2], [int(f) for f in sys.argv[3].split(',')]
clear = '--clear' in sys.argv
with open(os.path.join(ROOT, 'states', f'slot{src}.p2s'), 'rb') as f:
    head = f.read(24)
    ram = f.read(32 * 1024 * 1024)
vblank = struct.unpack('<IIIQ', head[:20])[3]
words = {}
for n in flags:
    byte = BASE + n // 8
    waddr = byte & ~3
    if waddr not in words:
        words[waddr] = struct.unpack_from('<I', ram, waddr - 0x08000000)[0]
    bit = (byte - waddr) * 8 + n % 8
    words[waddr] = words[waddr] & ~(1 << bit) if clear else words[waddr] | (1 << bit)
poke = f'{vblank + 1},' + ','.join(f'{a:08x},x{v:x}' for a, v in words.items())
env = dict(os.environ, P2_POKE=poke)
subprocess.run([EXE, '--headless', '--load-state', src, '--turbo-until', '99999999', '--save-state-at', f'{vblank + 4}:{dst}',
                '--exit-after-save', '--exit-after', str(vblank + 100)], env=env, cwd=ROOT, capture_output=True)
print(f'slot {dst}: flags {flags} {"cleared" if clear else "set"} ({poke})')
