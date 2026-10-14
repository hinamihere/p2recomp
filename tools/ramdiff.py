"""Find the player's world position in guest RAM by comparing savestates taken after
moving in different directions from the same starting state.

usage: ramdiff.py <base> <right> <left> <up> <down>
Prints float32 candidates whose value moved opposite ways for right/left (or up/down) and
stayed put in the base state.
"""
import sys

import numpy as np

RAM_OFF = 20 + 4  # header (magic, version, gp, vblank) + "MEM " tag
RAM_BASE = 0x08000000
RAM_SIZE = 32 * 1024 * 1024


def ram(slot):
    with open(f"states/slot{slot}.p2s", "rb") as f:
        f.seek(RAM_OFF)
        return np.frombuffer(f.read(RAM_SIZE), dtype="<f4")


def main():
    base, right, left, up, down = (ram(s) for s in sys.argv[1:6])
    with np.errstate(invalid="ignore", over="ignore"):
        ok = np.isfinite(base) & (np.abs(base) < 1e5) & (np.abs(base) > 1e-3)
        for name, a, b in (("right/left", right, left), ("up/down", up, down)):
            da, db = a - base, b - base
            hit = ok & (np.abs(da) > 1) & (np.abs(db) > 1) & (np.sign(da) == -np.sign(db)) & (np.abs(da) < 2000) & (np.abs(db) < 2000)
            idx = np.nonzero(hit)[0]
            print(f"== {name}: {len(idx)} candidates")
            for i in idx[:40]:
                print(f"  {RAM_BASE + 4 * i:08x} base={base[i]:10.2f} {name.split('/')[0]}={a[i]:10.2f} {name.split('/')[1]}={b[i]:10.2f}")


if __name__ == "__main__":
    main()
