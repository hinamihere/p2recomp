"""Hold one d-pad direction from a savestate and report the frames where the party leader glows purple (an interaction is
available), as a contact sheet.  usage: glowscan.py <slot> <BUTTON> <frames> <out.png>"""
import glob, os, struct, subprocess, sys
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("P2_EXE", os.path.join(ROOT, "build", "dev", "p2.exe"))
slot, button, frames, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
v0 = struct.unpack("<IIIQ", open(os.path.join(ROOT, "states", f"slot{slot}.p2s"), "rb").read(20))[3]
for f in glob.glob(os.path.join(ROOT, "build", "frames", "*.bmp")):
    os.remove(f)
env = dict(os.environ, P2_DUMP_VBLANK="1", P2_NO_ENCOUNTERS="1")
subprocess.run([EXE, "--headless", "--load-state", slot, "--turbo-until", "99999999", "--exit-after", str(v0 + frames + 40),
                "--dump-frames", "2", "--input", f"{v0 + 10}={button}:{frames}"], env=env, cwd=ROOT, capture_output=True)
files = sorted(glob.glob(os.path.join(ROOT, "build", "frames", "frame_*.bmp")))
glow = []
for f in files:
    im = Image.open(f).convert("RGB")
    n = sum(1 for r, g, b in im.crop((0, 20, 400, 200)).getdata() if r > 110 and b > 110 and g < 70 and abs(r - b) < 70)
    if n > 25:
        glow.append((f, n))
print(len(files), "frames,", len(glow), "glowing")
sel = glow[:: max(1, len(glow) // 12)][:12]
W = Image.new("RGB", (4 * 240, 3 * 136))
for i, (f, n) in enumerate(sel):
    W.paste(Image.open(f).convert("RGB").resize((240, 136)), ((i % 4) * 240, (i // 4) * 136))
    print(os.path.basename(f), n)
W.save(out)
