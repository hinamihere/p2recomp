"""Random walks from a savestate in a dungeon until a battle starts (detected by the enemy-encounter
flash / battle menu: a large share of the frame in the battle UI's dark-red command bar).
usage: battlehunt.py <from-slot> <to-slot> <first-seed> <tries> [vblanks]"""
import os, struct, subprocess, sys
import numpy as np
from PIL import Image
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import talkhunt  # noqa: E402

def main():
    src, dst, first, tries = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    length = int(sys.argv[5]) if len(sys.argv) > 5 else 3000
    start = struct.unpack("<IIIQ", open(os.path.join(ROOT, "states", f"slot{src}.p2s"), "rb").read(20))[3]
    frames = os.path.join(ROOT, "build", "frames")
    for seed in range(first, first + tries):
        walk = talkhunt.walk(seed, length).replace("=CROSS:", "=CIRCLE:")
        inp = ",".join(f"{start + int(p.split('=')[0])}={p.split('=')[1]}" for p in walk.split(","))
        subprocess.run(["rm", "-rf", frames])
        env = dict(os.environ, P2_DUMP_VBLANK="1")
        subprocess.run([os.path.join(ROOT, "build", "release", "p2.exe"), "--headless", "-q", "--load-state", str(src), "--input", inp,
                        "--turbo-until", "99999999", "--exit-after", str(start + length), "--dump-frames", "2"], cwd=ROOT, capture_output=True, env=env)
        first_hit = None
        for n in sorted(os.listdir(frames)):
            a = np.asarray(Image.open(os.path.join(frames, n)).convert("RGB")).astype(int)
            # bright flash / white-out frames or a battle command window: dark-red box at bottom with text
            if (a.mean() > 200) or False:
                first_hit = n; break
        print(f"seed {seed}: {'flash at ' + first_hit if first_hit else 'no flash'}")
        if first_hit:
            open(os.path.join(ROOT, "build", "battle_script.txt"), "w").write(inp)
            return

if __name__ == "__main__":
    main()
