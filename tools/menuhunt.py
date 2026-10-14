"""Random walks from a savestate until a frame shows a shopping-mall/destination menu
(its bright green selection box), e.g. to leave a shop.

usage: menuhunt.py <from-slot> <to-slot> <first-seed> <tries> [vblanks]
"""
import os
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import talkhunt  # noqa: E402


def menu_frame(path):
    a = np.asarray(Image.open(path).convert("RGB")).astype(int)[40:220, 0:400]
    green = (a[..., 1] > 200) & (a[..., 0] < 190) & (a[..., 2] < 120)
    return green.sum() > 2000


def main():
    src, dst, first, tries = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    length = int(sys.argv[5]) if len(sys.argv) > 5 else 1500
    frames = os.path.join(ROOT, "build", "frames")
    for seed in range(first, first + tries):
        # Circle first closes any open dialogue; no Cross so nothing is selected in menus.
        walk = talkhunt.walk(seed, length)
        if os.environ.get("MENUHUNT_CROSS") is None:
            walk = walk.replace("=CROSS:", "=CIRCLE:")
        else:
            # Keep Cross (doors need it) but close whatever it opens right after.
            parts = []
            for p in walk.split(","):
                parts.append(p)
                t, b = p.split("=")
                if b.startswith("CROSS"):
                    parts += [f"{int(t) + 6}=CIRCLE:3", f"{int(t) + 12}=CIRCLE:3"]
            walk = ",".join(parts)
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "drive.py"), src, str(dst), str(length), walk],
                       cwd=ROOT, check=False, capture_output=True)
        hits = [n for n in sorted(os.listdir(frames)) if menu_frame(os.path.join(frames, n))]
        if hits:
            Image.open(os.path.join(frames, hits[0])).save(os.path.join(ROOT, "build", "drive", f"menu_{seed}.png"))
            print(f"seed {seed}: menu at {hits[0]} ({len(hits)} frames), state in slot {dst}")
            return
        print(f"seed {seed}: nothing")


if __name__ == "__main__":
    main()
