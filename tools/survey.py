"""Random walks (with Cross) from a savestate, collecting every distinct location banner and
dialogue window seen, as one montage per run (build/drive/survey_<seed>.png).

usage: survey.py <from-slot> <to-slot> <first-seed> <tries> [vblanks]
"""
import os
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import talkhunt  # noqa: E402

BANNER = (0, 0, 200, 26)
DIALOG = (0, 196, 480, 272)


def text_mask(img, box):
    return np.asarray(img.convert("L").crop(box)) > 200


def collect(frames):
    banners, dialogs = [], []
    for n in sorted(os.listdir(frames)):
        img = Image.open(os.path.join(frames, n)).convert("RGB")
        a = np.asarray(img).astype(int)
        row = a[192:194, 20:460]
        is_dialog = ((row[..., 0] > 120) & (row[..., 1] < 60) & (row[..., 2] < 60)).mean() > 0.8
        if is_dialog:
            m = text_mask(img, DIALOG)
            if m.sum() > 200 and all((m != d[0]).mean() > 0.02 for d in dialogs):
                dialogs.append((m, img.crop(DIALOG)))
        else:
            # Banner text is white on a dark red plate; outdoors the plate is translucent,
            # so only pixels that are near-white in all channels count.
            px = np.asarray(img.crop(BANNER)).astype(int)
            m = (px.min(axis=2) > 215)
            if m.sum() > 50 and all((m != b[0]).mean() > 0.03 for b in banners):
                banners.append((m, img.crop(BANNER)))
    return [b[1] for b in banners], [d[1] for d in dialogs]


def montage(banners, dialogs, path):
    h = 26 * len(banners) + 76 * len(dialogs) + 4
    out = Image.new("RGB", (480, max(h, 30)))
    y = 0
    for b in banners:
        out.paste(b, (0, y))
        y += 26
    for d in dialogs:
        out.paste(d, (0, y))
        y += 76
    out.save(path)


def main():
    src, dst, first, tries = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    length = int(sys.argv[5]) if len(sys.argv) > 5 else 3000
    frames = os.path.join(ROOT, "build", "frames")
    for seed in range(first, first + tries):
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "drive.py"), src, str(dst), str(length),
                        talkhunt.walk(seed, length)], cwd=ROOT, check=True, capture_output=True)
        banners, dialogs = collect(frames)
        montage(banners, dialogs, os.path.join(ROOT, "build", "drive", f"survey_{seed}.png"))
        print(f"seed {seed}: {len(banners)} banners, {len(dialogs)} dialogue frames")


if __name__ == "__main__":
    main()
