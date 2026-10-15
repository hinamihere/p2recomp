"""Try one input recipe for several values of a parameter in parallel and report which ones open a dialogue box.

usage: sweep.py <slot> "<recipe with {n}>" <n1,n2,...> <out.png> [after-vblanks=300]
The recipe uses relative vblanks like drive.py, e.g. "10=LEFT:{n},130=UP:6,145=CROSS:3".
A dialogue box is detected by the red separator line at screen row ~192 (like the bot's dialogue_visible).
"""
import concurrent.futures
import glob
import os
import shutil
import struct
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("P2_EXE", os.path.join(ROOT, "build", "dev", "p2.exe"))
slot, recipe, values, out = sys.argv[1], sys.argv[2], sys.argv[3].split(","), sys.argv[4]
after = int(sys.argv[5]) if len(sys.argv) > 5 else 300
v0 = struct.unpack("<IIIQ", open(os.path.join(ROOT, "states", f"slot{slot}.p2s"), "rb").read(20))[3]


def absolute(spec):
    parts = []
    for p in spec.split(","):
        t, rest = p.split("=", 1)
        parts.append(f"{v0 + int(t)}={rest}")
    return ",".join(parts)


def has_dialogue(path):
    im = Image.open(path).convert("RGB")
    red = 0
    for y in (191, 192, 193):
        for x in range(20, 460, 4):
            r, g, b = im.getpixel((x, y))
            if r > 110 and g < 70 and b < 70:
                red += 1
    return red > 60


def run(n):
    d = os.path.join(ROOT, "build", "sweep", n)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "build", "frames"))
    # the exe writes frames to build/frames relative to its cwd and reads bin/, extracted/ via cwd too: use junction-free approach
    return n


def run_one(n):
    rel = f"build/sweep/f{n}"  # relative: the exe cannot open non-ASCII absolute paths
    frames_dir = os.path.join(ROOT, rel)
    shutil.rmtree(frames_dir, ignore_errors=True)
    os.makedirs(frames_dir)
    env = dict(os.environ, P2_DUMP_VBLANK="1", P2_NO_ENCOUNTERS="1", P2_FRAMES_DIR=rel)
    spec = recipe.replace("{n}", n)
    last = max(int(p.split("=")[0]) for p in spec.split(","))
    subprocess.run([EXE, "--headless", "--load-state", slot, "--turbo-until", str(v0 + last - 20), "--exit-after", str(v0 + last + after),
                    "--dump-frames", "3", "--input", absolute(spec)], env=env, cwd=ROOT, capture_output=True)
    frames = sorted(glob.glob(os.path.join(frames_dir, "frame_*.bmp")))
    hit = [f for f in frames if has_dialogue(f)]
    return n, frames, hit


with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
    results = list(ex.map(run_one, values))
sheet = Image.new("RGB", (len(values) * 240, 136))
for i, (n, frames, hit) in enumerate(results):
    print(n, len(frames), "frames,", len(hit), "with a dialogue box")
    pick = hit[0] if hit else (frames[-1] if frames else None)
    if pick:
        sheet.paste(Image.open(pick).convert("RGB").resize((240, 136)), (i * 240, 0))
sheet.save(out)
