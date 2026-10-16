"""Dump the room scene script resident in a savestate: labels, character placements (op 0x31: id, model, x, y, z, dir),
party placement (op 0x2c), label bindings (op 0x46) and the story flags each label tests/sets.

usage: roomscript.py <slot> [dis <label>]
The resident copy has absolute pointers in its header (count/index/code/text); they are rebased before parsing with
tools/evscene.Script. The leader's room position is the int32 triple at 0x08E44C14."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import evscene  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(slot):
    with open(os.path.join(ROOT, 'states', f'slot{slot}.p2s'), 'rb') as f:
        f.seek(24)
        return f.read(0x2000000)


def find_scripts(R):
    i = R.find(b'START\x00', 0x1000000)
    while i != -1:
        base = i - 0x18
        hdr = struct.unpack_from('<6I', R, base)
        A = base + 0x08000000
        if hdr[0] == A + 0x18 and 0 < hdr[2] < 64 and A < hdr[3] < hdr[4] < hdr[5] < A + 0x40000:
            yield base, A, hdr
        i = R.find(b'START\x00', i + 1)


def parse(R, base, A, hdr):
    p = bytearray(R[base:base + (hdr[5] - A) + 0x8000])
    for k in (0, 1, 3, 4, 5):
        struct.pack_into('<I', p, 4 * k, hdr[k] - A)
    idx0, code0 = hdr[3] - A, hdr[4] - A
    for k in range((code0 - idx0) // 8):
        a, b = struct.unpack_from('<II', p, idx0 + 8 * k)
        if b >= A:
            struct.pack_into('<II', p, idx0 + 8 * k, a, b - A)
    return evscene.Script(bytes(p))


def main():
    R = load(sys.argv[1])
    print('leader room pos', struct.unpack_from('<3i', R, 0xE44C14))
    for base, A, hdr in find_scripts(R):
        s = parse(R, base, A, hdr)
        print(f'script at {A:08x}: labels {s.names}')
        if len(sys.argv) > 3 and sys.argv[2] == 'dis':
            lab = {v: k for k, v in s.labels.items()}[sys.argv[3]]
            ends = sorted(k for k in s.labels if k > lab) + [len(s.ins)]
            s.disassemble(lab, ends[0])
            continue
        starts = sorted(s.labels)
        for n, (op, args) in enumerate(s.ins):
            if op == 0x2c:
                print('  party at', args)
            elif op == 0x31:
                print('  char', args[0], 'model', args[1], 'pos', (args[2], args[4]), 'dir', args[5])
            elif op == 0x46:
                print('  bind char', args[0], '-> label', s.names[args[1]] if isinstance(args[1], int) and args[1] < len(s.names) else args[1])
        for k, st in enumerate(starts):
            en = starts[k + 1] if k + 1 < len(starts) else len(s.ins)
            tests = [a[1] for op, a in s.ins[st:en] if op == 0x60]
            sets = [a[0] for op, a in s.ins[st:en] if op == 0x5e]
            if tests or sets:
                print(f'  {s.labels[st]}: tests {tests} sets {sets}')


if __name__ == '__main__':
    main()
