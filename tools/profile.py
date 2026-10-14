"""Symbolize build/profile_raw.txt written by a P2_PROFILE=1 run.

usage: profile.py <p2.exe> [top]

Host functions are resolved with addr2line. Samples in recompiled guest code are, when the
generated units carry line info (-g1, see build/profile), attributed to the guest function
whose label precedes the sampled source line.
"""
import bisect
import collections
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def image_base(exe):
    out = subprocess.run(["objdump", "-p", exe], capture_output=True, text=True).stdout
    m = re.search(r"ImageBase\s+([0-9a-fA-F]+)", out)
    return int(m.group(1), 16)


def guest_label_index(path, cache={}):
    """Sorted (line, label) pairs for L_xxxxxxxx labels and function starts in a generated unit."""
    if path in cache:
        return cache[path]
    lines, labels = [], []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for i, text in enumerate(f, 1):
                m = re.match(r"\s*L_([0-9A-F]{8}):", text)
                if m:
                    lines.append(i)
                    labels.append(m.group(1))
    except OSError:
        pass
    cache[path] = (lines, labels)
    return cache[path]


def main():
    exe = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    raw = os.path.join(ROOT, "build", "profile_raw.txt")
    entries = []
    total = 0
    exe_name = os.path.basename(exe).lower()
    for line in open(raw):
        if line.startswith("#"):
            continue
        rva, count, module = line.split(" ", 2)
        entries.append((int(rva, 16), int(count), module.strip()))
        total += int(count)

    base = image_base(exe)
    mine = [e for e in entries if os.path.basename(e[2]).lower() == exe_name]
    other = collections.Counter()
    for rva, count, module in entries:
        if os.path.basename(module).lower() != exe_name:
            other[os.path.basename(module)] += count

    addrs = "\n".join(hex(base + rva) for rva, _, _ in mine)
    out = subprocess.run(["addr2line", "-f", "-C", "-e", exe], input=addrs, capture_output=True, text=True).stdout.splitlines()
    funcs = collections.Counter()
    guest = collections.Counter()
    for i, (rva, count, _) in enumerate(mine):
        func, loc = out[2 * i], out[2 * i + 1]
        func = re.sub(r"\(.*", "", func)
        funcs[func] += count
        m = re.match(r"(.*generated_unit_\d+\.cpp):(\d+)", loc)
        if m:
            lines, labels = guest_label_index(m.group(1))
            k = bisect.bisect_right(lines, int(m.group(2))) - 1
            guest[labels[k] if k >= 0 else "?"] += count

    print(f"{total} samples")
    print("\n== host functions")
    for name, count in funcs.most_common(top):
        print(f"{100.0 * count / total:6.2f}%  {name}")
    print("\n== other modules")
    for name, count in other.most_common(10):
        print(f"{100.0 * count / total:6.2f}%  {name}")
    if guest:
        print("\n== guest code by label (nearest preceding L_ label)")
        for label, count in guest.most_common(top):
            print(f"{100.0 * count / total:6.2f}%  {label}")


if __name__ == "__main__":
    main()
