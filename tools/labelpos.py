"""Cursor positions (guest 08cd3c50/54) at which a place label with centroid near (lx,ly) is shown.
usage: labelpos.py <from-slot> <first-seed> <tries> <lx> <ly> [vblanks]"""
import os, struct, subprocess, sys
import numpy as np
from PIL import Image
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import labelhunt  # noqa: E402

def main():
    slot, first, tries, lx, ly = [int(x) for x in sys.argv[1:6]]
    length = int(sys.argv[6]) if len(sys.argv) > 6 else 3000
    start = struct.unpack("<IIIQ", open(os.path.join(ROOT, "states", f"slot{slot}.p2s"), "rb").read(20))[3]
    frames = os.path.join(ROOT, "build", "frames")
    pos = {}
    env = dict(os.environ, P2_WATCH="08cd3c50,08cd3c54")
    for seed in range(first, first + tries):
        inp = labelhunt.script(seed, start, length)
        subprocess.run(["rm", "-rf", frames])
        r = subprocess.run([labelhunt.EXE, "--headless", "--load-state", str(slot), "--input", inp, "--exit-after", str(start + length),
                            "--dump-frames", "1"], cwd=ROOT, capture_output=True, text=True, env=env)
        track = {}
        for line in r.stdout.splitlines() + r.stderr.splitlines():
            if " watch " in line:
                p = line.split()
                track[int(p[1])] = (float(p[4]), float(p[5]))
        for i, n in enumerate(sorted(os.listdir(frames))):
            a = np.asarray(Image.open(os.path.join(frames, n)).convert("RGB")).astype(int)[30:250, 0:410]
            red = (a[..., 0] > 150) & (a[..., 1] < 60) & (a[..., 2] < 60)
            if red.sum() > 150:
                ys, xs = np.nonzero(red)
                if abs(xs.mean() - lx) < 18 and abs(ys.mean() + 30 - ly) < 18:
                    v = start + i
                    if v in track:
                        pos.setdefault(track[v], []).append((seed, v))
    for p, hits in sorted(pos.items()):
        print(p, len(hits), hits[0])

main()
