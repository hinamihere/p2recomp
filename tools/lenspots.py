"""For several LEFT-walk lengths along the classroom wall: walk, face the wall, press Cross three times; sheet of the frame after the first Cross.
usage: lenspots.py <slot> <out.png> <len,len,...> [walkdir=LEFT] [facedir=UP+LEFT] [prewalk='10=DOWN:40']"""
import glob, os, struct, subprocess, sys
from PIL import Image
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("P2_EXE", os.path.join(ROOT, "build", "dev", "p2.exe"))
slot, out, ns = sys.argv[1], sys.argv[2], [int(v) for v in sys.argv[3].split(",")]
walk = sys.argv[4] if len(sys.argv) > 4 else "LEFT"
face = sys.argv[5] if len(sys.argv) > 5 else "UP+LEFT"
pre = sys.argv[6] if len(sys.argv) > 6 else "10=DOWN:40"
v0 = struct.unpack("<IIIQ", open(os.path.join(ROOT, "states", f"slot{slot}.p2s"), "rb").read(20))[3]
procs = []
for n in ns:
    rel = f"build/sweep/g{n}"
    os.makedirs(os.path.join(ROOT, rel), exist_ok=True)
    for f in os.listdir(os.path.join(ROOT, rel)):
        os.remove(os.path.join(ROOT, rel, f))
    env = dict(os.environ, P2_DUMP_VBLANK="1", P2_NO_ENCOUNTERS="1", P2_FRAMES_DIR=rel)
    r = [pre, f"60={walk}:{n}", f"{60 + n + 10}={face}:30", f"{60 + n + 60}=CROSS:3", f"{60 + n + 110}=CROSS:3", f"{60 + n + 160}=CROSS:3"]
    inp = ",".join(f"{v0 + int(p.split('=')[0])}={p.split('=', 1)[1]}" for p in ",".join(r).split(","))
    last = 60 + n + 160
    procs.append(subprocess.Popen([EXE, "--headless", "--load-state", slot, "--turbo-until", str(v0 + last - 20), "--exit-after", str(v0 + last + 60),
                                   "--dump-frames", "3", "--input", inp], env=env, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
for p in procs:
    p.wait()
W = Image.new("RGB", (len(ns) * 240, 272))
for i, n in enumerate(ns):
    fs = sorted(glob.glob(os.path.join(ROOT, f"build/sweep/g{n}/*.bmp")))
    if not fs:
        continue
    t = v0 + 60 + n + 60 + 45
    for row, dt in enumerate((0, 50)):
        f = min(fs, key=lambda x: abs(int(x[-10:-4]) - (t + dt)))
        W.paste(Image.open(f).convert("RGB").resize((240, 136)), (i * 240, row * 136))
W.save(out)
