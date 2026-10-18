"""Write a self-contained work packet for decompiling one function (hand it to any agent or person).

usage: packet.py <addr> [out-dir=build/packets]       one function
       packet.py --next <n> [--tier S|M|L|XL] [--leaf]  the next n todo functions (smallest first), one packet each

A packet (build/packets/<ADDR>.md, not committed: it contains disassembly of the game) holds the function's bounds and
call-graph data from docs/decomp/functions.csv, its disassembly (tools/showfn.py: unrelocated words plus the real
constants from the generated C++), its callers and callees with their status, and the rules for a replacement.
"""
import csv
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSV = os.path.join(ROOT, 'docs', 'decomp', 'functions.csv')

RULES = """## Task

Write a C++ replacement for this function that behaves **exactly** like the original: the same return value in v0/v1,
the same writes to guest memory, the same calls to other guest functions with the same arguments in the same order.
Readable code with meaningful names is the point. Do not translate it instruction by instruction.

Deliverable: a PR that touches only these files.
- `game/src/native_funcs.cpp`: the function inside `namespace p2::game`, plus its binding at the bottom:
  `P2_NATIVE({addr}, p2::game::<name>)` (returns a value in v0) or `P2_NATIVE_VOID({addr}, p2::game::<name>)`.
- `game/overrides.txt`: the line `{addr}  # <name>`.
- Optional: notes in `docs/decomp/` (structs, what the function is for).

How replacements are written (see the existing ones in `game/src/native_funcs.cpp`, e.g. `game_rand`, `pad_held`,
`map_cursor_pick_edge`):
- Arguments arrive from a0..a3 (then the stack) by their C++ parameter types. Guest pointers are `u32` guest addresses.
- Guest memory goes through `guest<T>(addr)`; fixed globals are `Global<T, 0xADDR>`; other guest functions are called
  with `guest_call(addr, args...)`. Do not call host APIs, keep no host-side state, do not reorder memory writes in a way
  another thread could observe.
- MIPS details: `lui`+`addiu` sign-extend the low half; `sltu`/`slt` signedness matters; branch-likely (`beql`, `bnel`)
  run their delay slot only when taken; a load right before a branch is visible in the delay slot; div by zero and
  overflowing shifts must behave as on MIPS (`u32` arithmetic, no UB).

How it is checked (CI, docs/decomp/ci.md): 200 calls with random arguments and RAM, original vs yours on identical
state (all registers and all RAM must match), then 24 savestate windows over the whole game, byte-identical.
Run it yourself on the maintainer's machine: `bash tools/ci_check.sh origin/master`.
"""


def load():
    return {r['addr']: r for r in csv.DictReader(open(CSV, encoding='utf-8'))}


def callers_of(addr, funcs):
    """Call sites of addr and the functions containing them (scans the disassembly)."""
    import bisect
    asm = os.path.join(ROOT, 'build', 'full_lf.asm')
    starts = sorted(int(a, 16) for a in funcs)
    target = int(addr, 16)
    out = []
    with open(asm, encoding='utf-8', errors='replace') as f:
        for line in f:
            if ' jal ' in line or '\tjal' in line or 'jal  ' in line:
                parts = line.split()
                if len(parts) > 2 and parts[2] == 'jal':
                    word = int(parts[1], 16)
                    if ((word & 0x3FFFFFF) << 2) + 0x08804000 == target:
                        pc = int(parts[0].rstrip(':'), 16)
                        i = bisect.bisect_right(starts, pc) - 1
                        out.append((pc, starts[i] if i >= 0 else 0))
    return out


def callees_of(addr, n, funcs):
    asm = os.path.join(ROOT, 'build', 'full_lf.asm')
    start, end = int(addr, 16), int(addr, 16) + 4 * n
    out = []
    with open(asm, encoding='utf-8', errors='replace') as f:
        for line in f:
            parts = line.split()
            if len(parts) > 2 and parts[2] == 'jal':
                pc = int(parts[0].rstrip(':'), 16)
                if start <= pc < end:
                    out.append(f'{((int(parts[1], 16) & 0x3FFFFFF) << 2) + 0x08804000:08X}')
    return out


def packet(addr, funcs, outdir):
    addr = addr.upper()
    r = funcs.get(addr)
    if r is None:
        sys.exit(f'{addr} is not a function start in {CSV}')
    n = int(r['instrs'])
    dis = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'showfn.py'), addr, str(n)], cwd=ROOT,
                         capture_output=True, text=True, errors='replace').stdout
    lines = [f'# Decompile {addr}', '',
             f'- size: {r["size"]} bytes ({n} instructions), size tier {r["tier"]}, found as `{r["kind"]}`',
             f'- leaf: {"yes" if r["leaf"] == "1" else "no"}, SDK/firmware calls: {r["stub_calls"]}', '']
    cs = callees_of(addr, n, funcs)
    if cs:
        lines += ['## Calls', '']
        for c in dict.fromkeys(cs):
            f = funcs.get(c, {})
            lines.append(f'- {c} ({f.get("instrs", "?")} instrs, {f.get("status", "?")}{", " + f["name"] if f.get("name") else ""})')
        lines.append('')
    cl = callers_of(addr, funcs)
    if cl:
        lines += ['## Called from', '']
        lines += [f'- {pc:08X} in {fn:08X}' for pc, fn in cl[:20]]
        if len(cl) > 20:
            lines.append(f'- ... {len(cl) - 20} more')
        lines.append('')
    lines += ['## Disassembly', '', '```', dis.rstrip(), '```', '', RULES.format(addr=addr)]
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, f'{addr}.md')
    open(path, 'w', encoding='utf-8').write('\n'.join(lines))
    return path


def main():
    args = sys.argv[1:]
    funcs = load()
    outdir = os.path.join(ROOT, 'build', 'packets')
    if args and args[0] == '--next':
        n = int(args[1])
        tier = args[args.index('--tier') + 1] if '--tier' in args else None
        leaf = '--leaf' in args
        todo = [r for r in funcs.values() if r['status'] == 'todo' and r['kind'] != 'prologue'
                and (tier is None or r['tier'] == tier) and (not leaf or r['leaf'] == '1')]
        todo.sort(key=lambda r: (int(r['instrs']), r['addr']))
        for r in todo[:n]:
            print(packet(r['addr'], funcs, outdir))
        return
    if not args:
        sys.exit(__doc__)
    print(packet(args[0], funcs, args[1] if len(args) > 1 else outdir))


if __name__ == '__main__':
    main()
