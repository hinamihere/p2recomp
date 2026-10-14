"""From a savestate on a town map, random-walk (turbo, reproducible) until the cursor is at a target
position (+/- tol), then save a state there.

usage: gotonode.py <from-slot> <to-slot> <x> <y> [tol=3] [max-seeds=30] [vblanks=6000]
"""
import os, random, struct, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "release", "p2.exe")

def script(seed, start, length):
    rng = random.Random(seed)
    dirs = ["UP", "DOWN", "LEFT", "RIGHT", "UP+LEFT", "UP+RIGHT", "DOWN+LEFT", "DOWN+RIGHT"]
    t, parts = 8, [f"{start + 2 + i * 40}=CIRCLE:4" for i in range(4)]
    t = 170
    while t < length:
        d = rng.randint(30, 200)
        parts.append(f"{start + t}={rng.choice(dirs)}:{d}")
        t += d + 2
    return ",".join(parts)

def main():
    src, dst, tx, ty = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
    tol = float(sys.argv[5]) if len(sys.argv) > 5 else 3
    seeds = int(sys.argv[6]) if len(sys.argv) > 6 else 30
    length = int(sys.argv[7]) if len(sys.argv) > 7 else 6000
    start = struct.unpack("<IIIQ", open(os.path.join(ROOT, "states", f"slot{src}.p2s"), "rb").read(20))[3]
    env = dict(os.environ, P2_WATCH_INT="1", P2_WATCH="08cd3c50,08cd3c54,08cd3c80")
    for seed in range(1, seeds + 1):
        inp = script(seed * 17, start, length)
        r = subprocess.run([EXE, "--headless", "--load-state", str(src), "--input", inp, "--turbo-until", "99999999",
                            "--exit-after", str(start + length)], cwd=ROOT, capture_output=True, text=True, env=env)
        hit = None
        for line in (r.stdout + r.stderr).splitlines():
            if " watch " not in line: continue
            p = line.split()
            x = struct.unpack("<f", struct.pack("<I", int(p[4])))[0]; y = struct.unpack("<f", struct.pack("<I", int(p[5])))[0]
            if abs(x - tx) <= tol and abs(y - ty) <= tol:
                hit = int(p[1]); break
        if hit is None:
            continue
        subprocess.run([EXE, "--headless", "-q", "--load-state", str(src), "--input", inp, "--turbo-until", "99999999",
                        "--exit-after", str(hit + 400), "--save-state-at", f"{hit + 8}:{dst}", "--exit-after-save"], cwd=ROOT, capture_output=True)
        print(f"seed {seed*17}: reached ({tx},{ty}) at vblank {hit}; state in slot {dst}")
        return
    print("not reached")

if __name__ == "__main__":
    main()
