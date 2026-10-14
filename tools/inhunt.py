"""Cursor positions at which the red 'IN' marker (enterable place node) shows on a town map.
usage: inhunt.py <from-slot> <first-seed> <tries> [vblanks]"""
import os, struct, subprocess, sys
import numpy as np
from PIL import Image
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import labelhunt  # noqa: E402

def _box_sum(m, h, w):
    c = np.pad(m.astype(np.int32).cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    return c[h:, w:] - c[:-h, w:] - c[h:, :-w] + c[:-h, :-w]


def find_in(a):
    """True if the red 'IN' marker (a ~13x9 dark-red box with bright-red letters) is in the frame."""
    dark = (np.abs(a[..., 0] - 43) < 6) & (a[..., 1] < 8) & (a[..., 2] < 8)
    bright = (np.abs(a[..., 0] - 216) < 12) & (a[..., 1] < 30) & (a[..., 2] < 30)
    dark[:24] = False
    bright[:24] = False
    d = _box_sum(dark, 9, 13)
    b = _box_sum(bright, 9, 13)
    return bool(((d > 60) & (d < 100) & (b > 18) & (b < 45)).any())


def main():
    slot, first, tries = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    length = int(sys.argv[4]) if len(sys.argv) > 4 else 3000
    start = struct.unpack("<IIIQ", open(os.path.join(ROOT, "states", f"slot{slot}.p2s"), "rb").read(20))[3]
    frames = os.path.join(ROOT, "build", "frames")
    tpl = np.asarray(Image.open(os.path.join(ROOT, "build", "drive", "slot442_in.png")).convert("RGB")).astype(int)
    th, tw = tpl.shape[:2]
    env = dict(os.environ, P2_WATCH="08cd3c50,08cd3c54")
    found = {}
    for seed in range(first, first + tries):
        inp = labelhunt.script(seed, start, length)
        subprocess.run(["rm", "-rf", frames])
        r = subprocess.run([labelhunt.EXE, "--headless", "--load-state", str(slot), "--input", inp, "--exit-after", str(start + length),
                            "--dump-frames", "2"], cwd=ROOT, capture_output=True, text=True, env=env)
        track = {}
        for line in (r.stdout + r.stderr).splitlines():
            if " watch " in line:
                p = line.split(); track[int(p[1])] = (int(float(p[4])), int(float(p[5])))
        for i, n in enumerate(sorted(os.listdir(frames))):
            a = np.asarray(Image.open(os.path.join(frames, n)).convert("RGB")).astype(int)
            if not find_in(a):
                continue
            if True:
                v = start + i * 2
                pos = track.get(v) or track.get(v + 1)
                if pos:
                    found.setdefault(pos, []).append((seed, v))
    for pos, h in sorted(found.items()):
        print(pos, len(h), h[0])

if __name__ == "__main__":
    main()
