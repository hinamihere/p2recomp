"""Random walks from a savestate on a town map until a place label appears at a given spot.

usage: labelhunt.py <from-slot> <first-seed> <tries> <label-x> <label-y> [vblanks]
Prints, per hit, the seed and the vblank (absolute) of the first frame showing the label. The
red label border's centroid must lie within 18 px of (label-x, label-y). Runs are real-time
(not fast-forwarded), so replaying the same input script reproduces them exactly.
"""
import os
import random
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "release", "p2.exe")


def script(seed, start, length):
    rng = random.Random(seed)
    dirs = ["UP", "DOWN", "LEFT", "RIGHT", "UP+LEFT", "UP+RIGHT", "DOWN+LEFT", "DOWN+RIGHT"]
    t, parts = 8, []
    while t < length:
        d = rng.randint(30, 160)
        parts.append(f"{start + t}={rng.choice(dirs)}:{d}")
        t += d + 2
    return ",".join(parts)


def main():
    slot, first, tries, lx, ly = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
    length = int(sys.argv[6]) if len(sys.argv) > 6 else 3000
    with open(os.path.join(ROOT, "states", f"slot{slot}.p2s"), "rb") as f:
        start = struct.unpack("<IIIQ", f.read(20))[3]
    frames = os.path.join(ROOT, "build", "frames")
    for seed in range(first, first + tries):
        inp = script(seed, start, length)
        subprocess.run(["rm", "-rf", frames])
        subprocess.run([EXE, "--headless", "-q", "--load-state", str(slot), "--input", inp, "--exit-after", str(start + length),
                        "--dump-frames", "1"], cwd=ROOT, capture_output=True)
        for i, n in enumerate(sorted(os.listdir(frames))):
            a = np.asarray(Image.open(os.path.join(frames, n)).convert("RGB")).astype(int)[30:250, 0:410]
            red = (a[..., 0] > 150) & (a[..., 1] < 60) & (a[..., 2] < 60)
            if red.sum() > 150:
                ys, xs = np.nonzero(red)
                if abs(xs.mean() - lx) < 18 and abs(ys.mean() + 30 - ly) < 18:
                    # frames are dumped one per presented vblank
                    print(f"seed {seed}: label at frame {i} (vblank ~{start + i}); script saved to build/label_{seed}.txt")
                    open(os.path.join(ROOT, "build", f"label_{seed}.txt"), "w").write(inp)
                    return
        print(f"seed {seed}: no label")


if __name__ == "__main__":
    main()
