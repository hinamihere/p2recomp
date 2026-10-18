"""Function list of the game executable (bin/EBOOT.dec), for planning and distributing the decomp work.

usage: funclist.py [out=docs/decomp/functions.csv]

Function starts:
  jal    target of a `jal` anywhere in .text
  ptr    target of a relocated 32-bit word in .rodata/.data/.ctors (function pointers, vtables, callback tables) that sits
         right after the end of another function (`jr $ra` + delay slot, optionally followed by nop padding); this rejects
         switch jump-table labels, which point into the middle of functions
  tail   target of a `j` that lands right after the end of another function (tail calls)
  prologue  unreferenced: a stack-frame prologue right after a function end (dead code or computed-table targets)
  entry  module entry / start of .text
A function extends to the next start (trailing nops trimmed). Compilers can place cold blocks between functions, so
sizes and caller counts are approximate. The CSV holds only addresses and call-graph metadata, no code.

Columns: addr, size (bytes), instrs, kind, callers (call sites), callees (distinct functions it jal's), stub_calls
(calls into the import stubs = Sony SDK/firmware calls), leaf (calls nothing), status (replaced if listed in
game/overrides.txt, otherwise todo), name (from overrides.txt), tier (size bucket: S <= 16 instrs, M <= 64, L <= 256, XL).
"""
import bisect
import csv
import os
import re
import struct
import sys

from elftools.elf.elffile import ELFFile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x08804000
JR_RA, NOP = 0x03E00008, 0x00000000


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'docs', 'decomp', 'functions.csv')
    elf = ELFFile(open(os.path.join(ROOT, 'bin', 'EBOOT.dec'), 'rb'))
    sec = {s.name: s for s in elf.iter_sections()}
    text = sec['.text']
    t0, tsize = text['sh_addr'], text['sh_size']
    tdata = text.data()
    words = struct.unpack(f'<{tsize // 4}I', tdata)
    stub = sec['.sceStub.text']
    s0, s1 = stub['sh_addr'], stub['sh_addr'] + stub['sh_size']
    word_at = lambda off: words[(off - t0) // 4] if t0 <= off < t0 + tsize else None

    def after_end(off):
        """True if `off` follows a function end: jr ra + delay slot, then any nops."""
        p = off - 4
        while p >= t0 + 4 and word_at(p) == NOP and word_at(p - 4) != JR_RA:
            p -= 4
        return p >= t0 + 4 and word_at(p - 4) == JR_RA

    starts, kinds, sites = {t0: 'entry'}, {}, {}
    callees, stub_calls = {}, {}
    jal_sites, j_targets = [], set()
    for i, w in enumerate(words):
        op = w >> 26
        pc = t0 + 4 * i
        if op == 3:  # jal
            tgt = (w & 0x3FFFFFF) << 2
            jal_sites.append((pc, tgt))
        elif op == 2:  # j
            j_targets.add((w & 0x3FFFFFF) << 2)
    for pc, tgt in jal_sites:
        if t0 <= tgt < t0 + tsize:
            starts.setdefault(tgt, 'jal')
    # relocated pointers in data sections
    for name in ('.rodata', '.rodata.0001', '.data', '.data.0001', '.ctors', '.dtors'):
        if name not in sec:
            continue
        s = sec[name]
        d = s.data()
        for k in range(0, len(d) - 3, 4):
            v = struct.unpack_from('<I', d, k)[0]
            if t0 <= v < t0 + tsize and v % 4 == 0 and v not in starts and after_end(v):
                starts[v] = 'ptr'
    # pointers built in code: lui rX, hi ; addiu/ori rY, rX, lo (callbacks, thread entries, task tables). Unrelocated
    # values are offsets from 0, so they look like small constants; only targets right after a function end are taken.
    for i, w in enumerate(words[:-8]):
        if w >> 26 != 0x0F:
            continue
        rt, hi = (w >> 16) & 31, w & 0xFFFF
        for k in range(1, 8):
            v = words[i + k]
            op = v >> 26
            if op in (0x09, 0x0D) and (v >> 21) & 31 == rt:
                lo = v & 0xFFFF
                tgt = (hi << 16) + (lo - 0x10000 if op == 0x09 and lo & 0x8000 else lo)
                if t0 <= tgt < t0 + tsize and tgt % 4 == 0 and tgt not in starts and after_end(tgt):
                    starts[tgt] = 'ptr'
                break
            if (v >> 16) & 31 == rt and op not in (0x2B, 0x29, 0x28):  # rX overwritten by something else
                break
    for tgt in j_targets:
        if t0 <= tgt < t0 + tsize and tgt not in starts and after_end(tgt):
            starts[tgt] = 'tail'

    # unreferenced starts: a stack-frame prologue (addiu sp, sp, -n) right after a function end. Usually dead code or code
    # reached through computed tables; kept separate so it does not get merged into the function before it.
    for i in range(2, len(words)):
        if words[i - 2] != JR_RA:
            continue
        j = i
        while j < len(words) and words[j] == NOP:
            j += 1
        if j < len(words) and words[j] >> 16 == 0x27BD and words[j] & 0x8000:
            starts.setdefault(t0 + 4 * j, 'prologue')
    order = sorted(starts)
    for pc, tgt in jal_sites:
        i = bisect.bisect_right(order, pc) - 1
        f = order[i]
        if s0 <= tgt < s1:
            stub_calls[f] = stub_calls.get(f, 0) + 1
        elif tgt in starts:
            sites[tgt] = sites.get(tgt, 0) + 1
            callees.setdefault(f, set()).add(tgt)

    replaced = {}
    for line in open(os.path.join(ROOT, 'game', 'overrides.txt'), encoding='utf-8'):
        m = re.match(r'\s*([0-9A-Fa-f]{8})\s*(?:#\s*(.*))?', line)
        if m:
            replaced[int(m.group(1), 16)] = (m.group(2) or '').strip()

    rows = []
    for i, f in enumerate(order):
        end = order[i + 1] if i + 1 < len(order) else t0 + tsize
        e = end
        while e > f + 4 and word_at(e - 4) == NOP and word_at(e - 8) != JR_RA:
            e -= 4
        n = (e - f) // 4
        addr = BASE + f
        tier = 'S' if n <= 16 else 'M' if n <= 64 else 'L' if n <= 256 else 'XL'
        rows.append([f'{addr:08X}', e - f, n, starts[f], sites.get(f, 0), len(callees.get(f, ())), stub_calls.get(f, 0),
                     int(not callees.get(f) and not stub_calls.get(f)), 'replaced' if addr in replaced else 'todo',
                     replaced.get(addr, ''), tier])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w', newline='', encoding='utf-8') as fh:
        w = csv.writer(fh)
        w.writerow(['addr', 'size', 'instrs', 'kind', 'callers', 'callees', 'stub_calls', 'leaf', 'status', 'name', 'tier'])
        w.writerows(rows)
    by_kind, by_tier = {}, {}
    for r in rows:
        by_kind[r[3]] = by_kind.get(r[3], 0) + 1
        by_tier[r[10]] = by_tier.get(r[10], 0) + 1
    instrs = sum(r[2] for r in rows)
    print(f'{len(rows)} functions, {instrs} instructions in .text ({tsize // 4} words)')
    print('by kind:', by_kind)
    print('by size:', by_tier, '(S <=16, M <=64, L <=256 instrs)')
    print('leaf functions:', sum(r[7] for r in rows), ' replaced:', sum(r[8] == 'replaced' for r in rows))
    print('->', out)


if __name__ == '__main__':
    main()
