"""Show a guest function as disassembly plus the relocated constants of the generated C++.

usage: showfn.py <addr> [max-instrs]
The disassembly in build/full_lf.asm is unrelocated (lui/addiu pairs point into the wrong place); the
generated C++ has the real addresses, so both are printed.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE, SPAN = 0x08804000, 0x4000


def main():
    addr = int(sys.argv[1], 16)
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    out = []
    with open(os.path.join(ROOT, "build", "full_lf.asm"), encoding="utf-8", errors="replace", newline="") as f:
        on = 0
        for line in f:
            if line.startswith(f"{addr:08X}:"):
                on = limit
            if on:
                out.append(line.rstrip()[:96])
                on -= 1
    print("\n".join(out))
    unit = (addr - BASE) // SPAN
    path = os.path.join(ROOT, "generated", f"generated_unit_{unit:04d}.cpp")
    print(f"--- {path} (unrelocated constants above, real ones below)")
    text = open(path, encoding="utf-8", errors="replace").read().split("\n")
    start = next(i for i, l in enumerate(text) if l.startswith(f"L_{addr:08X}:") )
    shown = 0
    for l in text[start:]:
        if re.match(r"\s*(goto L_|jump_target|local_pc|if \(\+\+local|ctx\.pc = jump|return;)", l) or re.match(r"L_\w+:$", l):
            continue
        print(l[:150])
        shown += 1
        if shown >= limit * 2:
            break


if __name__ == "__main__":
    main()
