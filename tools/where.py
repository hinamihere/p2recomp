"""Print the dungeon position of savestates: cell (08ceb2e0/e4), floor byte (08ce819d), room position, and which decoded
chunk-0 files from build/*_c0.bin are resident in RAM (identifies the dungeon file, e.g. Smile Hirasaka F0106 vs F0107).

usage: where.py <slot> [slot ...]"""
import glob
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sigs = {}
for f in glob.glob(os.path.join(ROOT, 'build', '*_c0.bin')):
    d = open(f, 'rb').read()
    if len(d) > 0x200:
        sigs[os.path.basename(f)[:-7]] = d[0x400:0x440]
for s in sys.argv[1:]:
    with open(os.path.join(ROOT, 'states', f'slot{s}.p2s'), 'rb') as fh:
        fh.seek(24)
        R = fh.read(0x2000000)
    files = [n for n, sig in sigs.items() if R.find(sig, 0x1000000) != -1]
    print(s, 'cell', (R[0xCEB2E0], R[0xCEB2E4]), 'floor', R[0xCE819D], 'room', struct.unpack_from('<3i', R, 0xE44C14), 'files', files)
