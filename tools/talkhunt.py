"""Random walks with frequent Cross from a savestate until a run shows a dialogue box.

usage: talkhunt.py <from-slot> <to-slot> <first-seed> <tries> [vblanks]

Each try plays a random walk (from-slot -> to-slot). The first try whose frames show the
red dialogue-window border keeps its state in to-slot and the first such frame in
build/drive/talk_<seed>.png.
"""
import os
import random
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def walk(seed, length):
    rng = random.Random(seed)
    dirs = ["UP", "DOWN", "LEFT", "RIGHT", "UP+LEFT", "UP+RIGHT", "DOWN+LEFT", "DOWN+RIGHT"]
    t, parts = 4, ["0=CIRCLE:2"]
    while t < length - 40:
        d = rng.randint(15, 90)
        parts.append(f"{t}={rng.choice(dirs)}:{d}")
        t += d + 3
        parts.append(f"{t}=CROSS:4")
        t += 14
    # Leave enough time to page through what was said.
    parts += [f"{length - 36 + i * 9}=CROSS:3" for i in range(3)]
    return ",".join(parts)


def dialogue_frames(frames):
    hits = []
    for n in sorted(os.listdir(frames)):
        a = np.asarray(Image.open(os.path.join(frames, n)).convert("RGB")).astype(int)
        row = a[192:194, 20:460]
        red = (row[..., 0] > 120) & (row[..., 1] < 60) & (row[..., 2] < 60)
        if red.mean() > 0.8:
            hits.append(n)
    return hits


def main():
    src, dst, first, tries = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    length = int(sys.argv[5]) if len(sys.argv) > 5 else 2400
    frames = os.path.join(ROOT, "build", "frames")
    for seed in range(first, first + tries):
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "drive.py"), src, str(dst), str(length), walk(seed, length),
                        "--sheet", "8"], cwd=ROOT, check=True, capture_output=True)
        hits = dialogue_frames(frames)
        if hits:
            Image.open(os.path.join(frames, hits[0])).save(os.path.join(ROOT, "build", "drive", f"talk_{seed}.png"))
            print(f"seed {seed}: dialogue in {len(hits)} frames, state in slot {dst}")
            return
        print(f"seed {seed}: nothing")


if __name__ == "__main__":
    main()
