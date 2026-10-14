"""Random-walk exploration from a savestate: stops at the first frame whose location
banner (top-left) differs from the starting one and keeps that frame.

usage: explore.py <from-slot> <to-slot> <seed | input script> <vblanks>
"""
import os
import random
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BANNER = (0, 0, 190, 26)


def walk(seed, length):
    rng = random.Random(seed)
    dirs = ["UP", "DOWN", "LEFT", "RIGHT", "UP+LEFT", "UP+RIGHT", "DOWN+LEFT", "DOWN+RIGHT"]
    t, parts = 0, []
    while t < length:
        if rng.random() < 0.8:
            d = rng.randint(40, 240)
            parts.append(f"{t}={rng.choice(dirs)}:{d}")
            t += d + 4
        else:
            parts.append(f"{t}=CROSS:4")
            t += 30
    return ",".join(parts)


def main():
    src, dst, seed, length = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
    script = seed if "=" in seed else walk(int(seed), length)
    if "=" in seed:
        seed = "script"
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "drive.py"), src, str(dst), str(length), script],
                   cwd=ROOT, check=True, capture_output=True)
    frames = os.path.join(ROOT, "build", "frames")
    names = sorted(os.listdir(frames))
    ref = None
    for n in names:
        # The banner is translucent: compare only its (white) text.
        img = np.asarray(Image.open(os.path.join(frames, n)).convert("L").crop(BANNER)) > 200
        if img.sum() < 50:  # no banner (fade, menu)
            continue
        if ref is None:
            ref = img
            continue
        if (img != ref).mean() > 0.03:
            Image.open(os.path.join(frames, n)).save(os.path.join(ROOT, "build", "drive", f"explore_{seed}.png"))
            print(f"seed {seed}: banner changed at frame {n}")
            return
    print(f"seed {seed}: no change in {len(names)} frames")


if __name__ == "__main__":
    main()
