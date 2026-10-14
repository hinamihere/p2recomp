"""Run a dungeon random walk and list the area banners that appear (new rooms/floors reached).

usage: bannerscan.py <from-slot> <seed> <vblanks>   (seed 0 = use the in-game exploration bot, P2_EXPLORE)
Frames are dumped by vblank (P2_DUMP_VBLANK); every frame where the top-left location banner appears after
being absent is cropped into build/banners/<vblank>.png, and a sheet of the crops is written to
build/banners/sheet.png. State of the final frame is not saved (use drive.py with the same seed for that).
"""
import os
import shutil
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import dungeonwalk  # noqa: E402


def banner_present(img):
    a = np.asarray(img.convert("RGB")).astype(int)[2:26, 0:200]
    red = (a[..., 0] > 140) & (a[..., 1] < 70) & (a[..., 2] < 70)
    return red.mean() > 0.04 and a[4:22, 8:190].mean() < 120


def main():
    src, seed, length = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    start = struct.unpack("<IIIQ", open(os.path.join(ROOT, "states", f"slot{src}.p2s"), "rb").read(20))[3]
    env = dict(os.environ, P2_DUMP_VBLANK="1")
    extra = []
    if seed == 0:
        env.update(P2_EXPLORE="1", P2_EXPLORE_SEED=os.environ.get("P2_EXPLORE_SEED", "1"))
    else:
        walk = dungeonwalk.script(seed, length)
        extra = ["--input", ",".join(f"{start + int(p.split('=')[0])}={p.split('=')[1]}" for p in walk.split(","))]
    frames = os.path.join(ROOT, "build", "frames")
    shutil.rmtree(frames, ignore_errors=True)
    subprocess.run([os.path.join(ROOT, "build", "release", "p2.exe"), "--headless", "-q", "--load-state", src] + extra +
                   ["--turbo-until", "99999999", "--exit-after", str(start + length), "--dump-frames", "6"], cwd=ROOT, capture_output=True, env=env)
    out = os.path.join(ROOT, "build", "banners")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    prev, crops = False, []
    for n in sorted(os.listdir(frames)):
        img = Image.open(os.path.join(frames, n))
        now = banner_present(img)
        if now and not prev:
            v = int(n.split("_")[1].split(".")[0])
            crop = img.crop((0, 0, 200, 28))
            crop.save(os.path.join(out, f"{v}.png"))
            crops.append((v, crop))
        prev = now
    if crops:
        sheet = Image.new("RGB", (200 * 4, 30 * ((len(crops) + 3) // 4)))
        for i, (v, c) in enumerate(crops):
            sheet.paste(c, ((i % 4) * 200, (i // 4) * 30))
        sheet.save(os.path.join(out, "sheet.png"))
    print(f"{len(crops)} banner appearances: " + " ".join(str(v) for v, _ in crops))


if __name__ == "__main__":
    main()
