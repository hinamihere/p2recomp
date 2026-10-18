"""List the replacement entry addresses a change touches, for CI (fuzz only what changed).

usage: ci_changed.py <base-ref> [head-ref=HEAD]
Prints comma-separated hex addresses: entries added to game/overrides.txt plus every P2_NATIVE binding whose bound C++
function's text differs between base and head in game/src/native_funcs.cpp (or that is new). Prints nothing if none."""
import re
import subprocess
import sys

base = sys.argv[1]
head = sys.argv[2] if len(sys.argv) > 2 else 'HEAD'


def show(ref, path):
    r = subprocess.run(['git', 'show', f'{ref}:{path}'], capture_output=True, text=True, errors='replace')
    return r.stdout if r.returncode == 0 else ''


def bodies(src):
    """{function name: its definition text} for every top-level function in native_funcs.cpp (brace matching)."""
    out = {}
    for m in re.finditer(r'^[\w:<>,\s\*&]+?\b(\w+)\s*\([^;{)]*\)\s*(?:const\s*)?\{', src, re.M):
        name, i, depth = m.group(1), m.end(), 1
        while i < len(src) and depth:
            depth += {'{': 1, '}': -1}.get(src[i], 0)
            i += 1
        out.setdefault(name, src[m.start():i])
    return out


def bindings(src):
    """{address: bound function name} from P2_NATIVE / P2_NATIVE_VOID lines and hand-written p2_native_<ADDR> entries."""
    out = {}
    for m in re.finditer(r'^P2_NATIVE(?:_VOID)?\(([0-9A-Fa-f]{8}),\s*([\w:]+)\)', src, re.M):
        out[m.group(1).upper()] = m.group(2).split('::')[-1]
    for m in re.finditer(r'\bvoid p2_native_([0-9A-Fa-f]{8})\(', src):
        out.setdefault(m.group(1).upper(), f'p2_native_{m.group(1)}')
    return out


def overrides(src):
    return {l.split('#')[0].strip().upper() for l in src.splitlines() if re.match(r'\s*[0-9A-Fa-f]{8}\b', l)}


old_src, new_src = show(base, 'game/src/native_funcs.cpp'), show(head, 'game/src/native_funcs.cpp')
old_b, new_b = bodies(old_src), bodies(new_src)
old_bind, new_bind = bindings(old_src), bindings(new_src)
changed = overrides(show(head, 'game/overrides.txt')) - overrides(show(base, 'game/overrides.txt'))
for addr, name in new_bind.items():
    if old_bind.get(addr) != name or old_b.get(name) != new_b.get(name):
        changed.add(addr)
print(','.join(sorted(changed)))
