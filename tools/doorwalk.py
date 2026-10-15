"""Walk a corridor from a savestate pressing UP+Cross every few steps and collect a contact sheet of what each try shows.

usage: doorwalk.py <slot> <dir RIGHT|LEFT> <tries> <out.png> [step-frames=28]
"""
import glob
import os
import struct
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("P2_EXE", os.path.join(ROOT, "build", "dev", "p2.exe"))

slot, direction, tries, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
step = int(sys.argv[5]) if len(sys.argv) > 5 else 28
v0 = struct.unpack("<IIIQ", open(os.path.join(ROOT, "states", f"slot{slot}.p2s"), "rb").read(20))[3]
period = step + 86
presses, t = [], 20
for k in range(tries):
    presses.append(f"{v0 + t}={direction}:{step}")
    t += step + 6
    presses.append(f"{v0 + t}=UP:6")
    t += 10
    presses.append(f"{v0 + t}=CROSS:3")
    t += 70
cross_at = [20 + k * period + step + 16 for k in range(tries)]
for f in glob.glob(os.path.join(ROOT, "build", "frames", "*.bmp")):
    os.remove(f)
env = dict(os.environ, P2_DUMP_VBLANK="1", P2_NO_ENCOUNTERS="1")
r = subprocess.run([EXE, "--headless", "--load-state", slot, "--turbo-until", "99999999", "--exit-after", str(v0 + t + 10),
                "--dump-frames", "6", "--input", ",".join(presses)], env=env, cwd=ROOT, capture_output=True, text=True, errors="replace")
open(os.path.join(ROOT, "build", "doorwalk.log"), "w", encoding="utf-8").write(r.stdout + r.stderr)
frames = {int(f[-10:-4]): f for f in glob.glob(os.path.join(ROOT, "build", "frames", "frame_*.bmp"))}
keys = sorted(frames)
cols = 6
rows = (tries + cols - 1) // cols
sheet = Image.new("RGB", (cols * 240, rows * 136))
for i, c in enumerate(cross_at):
    near = min(keys, key=lambda x: abs(x - (v0 + c + 45)))
    sheet.paste(Image.open(frames[near]).convert("RGB").resize((240, 136)), ((i % cols) * 240, (i // cols) * 136))
sheet.save(out)
print("frames", len(keys), "sheet", out)
