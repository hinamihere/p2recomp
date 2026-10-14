"""Walk in one direction from a savestate and report the vblank ranges where the "!" interaction bubble is shown
above the party leader (standing in front of a door / person / object).

usage: bubblescan.py <from-slot> <DIRECTION> <frames-of-walking> [save-slot-base]
"""
import os
import shutil
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def bubble(img):
    a = np.asarray(img.convert("RGB")).astype(int)[52:80, 224:256]
    white = (a[..., 0] > 225) & (a[..., 1] > 225) & (a[..., 2] > 225)
    return white.sum() > 60


def main():
    src, direction, walk = sys.argv[1], sys.argv[2], int(sys.argv[3])
    start = struct.unpack("<IIIQ", open(os.path.join(ROOT, "states", f"slot{src}.p2s"), "rb").read(20))[3]
    frames = os.path.join(ROOT, "build", "frames")
    shutil.rmtree(frames, ignore_errors=True)
    env = dict(os.environ, P2_DUMP_VBLANK="1")
    subprocess.run([os.path.join(ROOT, "build", "release", "p2.exe"), "--headless", "-q", "--load-state", src, "--input",
                    f"{start + 10}={direction}:{walk}", "--turbo-until", "99999999", "--exit-after", str(start + walk + 30),
                    "--dump-frames", "2"], cwd=ROOT, capture_output=True, env=env)
    runs, cur = [], None
    for n in sorted(os.listdir(frames)):
        v = int(n.split("_")[1].split(".")[0])
        if bubble(Image.open(os.path.join(frames, n))):
            cur = [v, v] if cur is None else [cur[0], v]
        elif cur:
            runs.append(cur)
            cur = None
    if cur:
        runs.append(cur)
    print(f"{direction}: bubble at vblank ranges (start {start}):", [(a - start, b - start) for a, b in runs])


if __name__ == "__main__":
    main()
