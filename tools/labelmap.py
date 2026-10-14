"""Collect the distinct place-label positions seen during random walks on a map from a savestate.
usage: labelmap.py <from-slot> <first-seed> <tries> [vblanks]   -> build/drive/labels/*.png
"""
import os, struct, subprocess, sys
import numpy as np
from PIL import Image
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import labelhunt  # noqa: E402

def main():
    slot, first, tries = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    length = int(sys.argv[4]) if len(sys.argv) > 4 else 3000
    start = struct.unpack("<IIIQ", open(os.path.join(ROOT, "states", f"slot{slot}.p2s"), "rb").read(20))[3]
    frames = os.path.join(ROOT, "build", "frames")
    out = os.path.join(ROOT, "build", "drive", "labels"); os.makedirs(out, exist_ok=True)
    seen = []
    for seed in range(first, first + tries):
        inp = labelhunt.script(seed, start, length)
        subprocess.run(["rm", "-rf", frames])
        subprocess.run([labelhunt.EXE, "--headless", "-q", "--load-state", str(slot), "--input", inp, "--exit-after", str(start + length),
                        "--dump-frames", "2"], cwd=ROOT, capture_output=True)
        for i, n in enumerate(sorted(os.listdir(frames))):
            a = np.asarray(Image.open(os.path.join(frames, n)).convert("RGB")).astype(int)[30:250, 0:410]
            red = (a[..., 0] > 150) & (a[..., 1] < 60) & (a[..., 2] < 60)
            if red.sum() > 150:
                ys, xs = np.nonzero(red); c = (int(xs.mean()), int(ys.mean()) + 30)
                if all(abs(c[0]-s[0]) > 25 or abs(c[1]-s[1]) > 12 for s in seen):
                    seen.append(c)
                    Image.open(os.path.join(frames, n)).save(os.path.join(out, f"label_{c[0]}_{c[1]}_s{seed}_f{i*2}.png"))
                    print("new label at", c, "seed", seed, "frame", i * 2)
if __name__ == "__main__":
    main()
