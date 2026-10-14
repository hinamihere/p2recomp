"""Static call graph of the EBOOT from build/full_lf.asm (disasm of 0x08804000-0x08bd6000).

usage: callgraph.py build                 write build/callgraph.json
       callgraph.py info <addr>           size, callers, callees of the function at/containing <addr>
       callgraph.py leaves [max_instrs]   small functions that call nothing (good first rewrite targets)
       callgraph.py top [n]               most-called functions

The dump holds unrelocated jal words, so targets are word<<2 + 0x08804000. Function starts are jal targets; a function extends to the next start. Compilers interleave
cold blocks between functions, so sizes and caller attribution are approximate.
"""
import bisect
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, "build", "full_lf.asm")
OUT = os.path.join(ROOT, "build", "callgraph.json")
LINE = re.compile(r"^([0-9A-F]{8}): ([0-9A-F]{8})\s+(\S+)")


def build():
    jals, instr = {}, []
    with open(ASM, encoding="utf-8", errors="replace", newline="") as f:
        for text in f:
            m = LINE.match(text)
            if not m:
                continue
            pc, word, op = int(m.group(1), 16), int(m.group(2), 16), m.group(3)
            instr.append(pc)
            if op == "jal":
                jals.setdefault(((word & 0x3FFFFFF) << 2) + 0x08804000, []).append(pc)
    starts = sorted(jals)
    callees = {s: set() for s in starts}
    for target, sites in jals.items():
        for site in sites:
            i = bisect.bisect_right(starts, site) - 1
            if i >= 0:
                callees[starts[i]].add(target)
    data = {"starts": starts, "callers": {hex(t): sites for t, sites in jals.items()},
            "callees": {hex(s): sorted(c) for s, c in callees.items()}, "end": instr[-1] + 4}
    with open(OUT, "w") as f:
        json.dump(data, f)
    print(f"{len(starts)} functions, {sum(len(v) for v in jals.values())} call sites")


def load():
    with open(OUT) as f:
        return json.load(f)


def span(d, s):
    i = bisect.bisect_right(d["starts"], s)
    return (d["starts"][i] if i < len(d["starts"]) else d["end"]) - s


def info(addr):
    d = load()
    i = bisect.bisect_right(d["starts"], addr) - 1
    s = d["starts"][i]
    print(f"function {s:08X} size {span(d, s) // 4} instrs")
    sites = d["callers"].get(hex(s), [])
    print(f"called {len(sites)}x from:", " ".join(f"{x:08X}" for x in sites[:16]))
    print("calls:", " ".join(f"{x:08X}" for x in d["callees"].get(hex(s), [])))


def leaves(limit):
    d = load()
    out = []
    for s in d["starts"]:
        n = span(d, s) // 4
        if n <= limit and not d["callees"].get(hex(s)):
            out.append((len(d["callers"].get(hex(s), [])), s, n))
    for calls, s, n in sorted(out, reverse=True)[:60]:
        print(f"{s:08X} {n:3d} instrs, {calls} callers")


def top(n):
    d = load()
    for c, s in sorted(((len(v), int(k, 16)) for k, v in d["callers"].items()), reverse=True)[:n]:
        print(f"{s:08X} {c} callers, {span(d, s) // 4} instrs")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "build":
        build()
    elif cmd == "info":
        info(int(sys.argv[2], 16))
    elif cmd == "leaves":
        leaves(int(sys.argv[2]) if len(sys.argv) > 2 else 24)
    elif cmd == "top":
        top(int(sys.argv[2]) if len(sys.argv) > 2 else 30)
