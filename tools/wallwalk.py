"""Walk along a wall in small steps from a savestate; after each step face the wall and press Cross (then Cross+Circle to close), sheet of results.
usage: wallwalk.py <slot> <walk-dir e.g. DOWN+LEFT> <face-dir e.g. UP+LEFT> <tries> <out.png> [step=10]"""
import glob, os, struct, subprocess, sys
from PIL import Image
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("P2_EXE", os.path.join(ROOT, "build", "dev", "p2.exe"))
slot, walk, face, tries, out = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
step = int(sys.argv[6]) if len(sys.argv) > 6 else 10
v0 = struct.unpack("<IIIQ", open(os.path.join(ROOT, "states", f"slot{slot}.p2s"), "rb").read(20))[3]
presses, t, probes = [], 10, []
for k in range(tries):
    presses.append(f"{v0 + t}={walk}:{step}"); t += step + 8
    presses.append(f"{v0 + t}={face}:10"); t += 14
    presses.append(f"{v0 + t}=CROSS:3"); probes.append(t + 22); t += 30
    presses.append(f"{v0 + t}=CROSS:3"); t += 20
    presses.append(f"{v0 + t}=CIRCLE:3"); t += 20
rel = "build/wallwalk"
os.makedirs(os.path.join(ROOT, rel), exist_ok=True)
for f in glob.glob(os.path.join(ROOT, rel, "*.bmp")):
    os.remove(f)
env = dict(os.environ, P2_DUMP_VBLANK="1", P2_NO_ENCOUNTERS="1", P2_FRAMES_DIR=rel)
subprocess.run([EXE, "--headless", "--load-state", slot, "--turbo-until", "99999999", "--exit-after", str(v0 + t + 10), "--dump-frames", "3",
                "--input", ",".join(presses)], env=env, cwd=ROOT, capture_output=True)
frames = {int(f[-10:-4]): f for f in glob.glob(os.path.join(ROOT, rel, "frame_*.bmp"))}
keys = sorted(frames)
cols = 6
sheet = Image.new("RGB", (cols * 240, ((tries + cols - 1) // cols) * 136))
for i, p in enumerate(probes):
    near = min(keys, key=lambda x: abs(x - (v0 + p)))
    sheet.paste(Image.open(frames[near]).convert("RGB").resize((240, 136)), ((i % cols) * 240, (i // cols) * 136))
sheet.save(out)
print("ok", len(keys))
