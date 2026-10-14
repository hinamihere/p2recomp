"""Random walks (with Cross) from a savestate until a frame looks like an outdoor area
(a large share of green pixels, e.g. lawns and trees), to find exits from interiors.

usage: outdoors.py <from-slot> <to-slot> <first-seed> <tries> [vblanks]
"""
import os
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import talkhunt  # noqa: E402


def green_share(path):
    a = np.asarray(Image.open(path).convert("RGB")).astype(int)[30:150, 120:470]  # skip banner/minimap
    g = (a[..., 1] > a[..., 0] + 12) & (a[..., 1] > a[..., 2] + 12)
    return g.mean()


def main():
    src, dst, first, tries = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    length = int(sys.argv[5]) if len(sys.argv) > 5 else 3000
    frames = os.path.join(ROOT, "build", "frames")
    for seed in range(first, first + tries):
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "drive.py"), src, str(dst), str(length),
                        talkhunt.walk(seed, length)], cwd=ROOT, check=True, capture_output=True)
        for n in sorted(os.listdir(frames))[3:]:
            share = green_share(os.path.join(frames, n))
            if share > 0.2:
                Image.open(os.path.join(frames, n)).save(os.path.join(ROOT, "build", "drive", f"outdoors_{seed}.png"))
                print(f"seed {seed}: outdoors at {n} ({share:.2f}), state in slot {dst}")
                return
        print(f"seed {seed}: indoors")


if __name__ == "__main__":
    main()
