"""Tour the interaction spots of one map file from a savestate, one run per spot, and keep a frame of what appeared.
usage: tourspots.py <map> <start-slot> <out-prefix> [first] [last]
Outcome per spot is printed (DIALOGUE / AREA / nothing / TIMEOUT / NOROUTE); frames at build/tourspots/<prefix>_<i>.bmp
"""
import os, re, struct, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("P2_EXE", os.path.join(ROOT, "build", "dev", "p2.exe"))
mapfile, slot, prefix = sys.argv[1], sys.argv[2], sys.argv[3]
first = int(sys.argv[4]) if len(sys.argv) > 4 else 0
last = int(sys.argv[5]) if len(sys.argv) > 5 else 10 ** 6
v0 = struct.unpack("<IIIQ", open(os.path.join(ROOT, "states", f"slot{slot}.p2s"), "rb").read(20))[3]
spots = [l.split() for l in open(mapfile) if l.startswith("EV ")]
spots = [(int(p[1]), int(p[2]), int(p[3])) for p in spots if int(p[3]) < 4]
os.makedirs(os.path.join(ROOT, "build", "tourspots"), exist_ok=True)
done = []
for i, (x, z, d) in enumerate(spots):
    if i < first or i >= last:
        continue
    if any(max(abs(x - u), abs(z - v)) < 2 for u, v in done):
        continue
    done.append((x, z))
    rel = f"build/tourspots/{prefix}_{i}"
    os.makedirs(os.path.join(ROOT, rel), exist_ok=True)
    env = dict(os.environ, P2_NO_ENCOUNTERS="1", P2_TOUR=f"{mapfile},{i},{i + 1}", P2_TOUR_SPREAD="0", P2_DUMP_VBLANK="1", P2_FRAMES_DIR=rel)
    r = subprocess.run([EXE, "--headless", "--turbo-until", "99999999", "--load-state", slot, "--exit-after", str(v0 + 9000), "--dump-frames", "20"],
                       env=env, cwd=ROOT, capture_output=True, text=True, errors="replace")
    log = r.stdout + r.stderr
    out = "AREA" if "AREA CHANGE" in log else "DIALOGUE" if "DIALOGUE" in log else "TIMEOUT" if "timed out" in log else "NOROUTE" if "no known route" in log else "nothing"
    print(f"spot #{i} ({x},{z}) dir {d}: {out}", flush=True)
