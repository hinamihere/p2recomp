"""Interact with every (deduplicated) interaction spot of a mapper file, one run per spot, and report what happens.

usage: spots.py <map.txt> <from-slot> [spread=4] [first] [last]
Each run loads <from-slot>, walks to the spot (P2_TOUR with a one-spot range), presses Cross and watches: DIALOGUE (a text box
appeared), AREA (the position jumped: stairs/door; a savestate is kept in slot 950+i) or nothing. Results go to
build/spots_<map>.txt. Needs P2_NO_ENCOUNTERS and the map's own axes/cell (they are read from the map file).
"""
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "release", "p2.exe")


def main():
    path, slot = sys.argv[1], sys.argv[2]
    spread = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    first = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    last = int(sys.argv[5]) if len(sys.argv) > 5 else 10**9
    spots = []
    for line in open(path):
        p = line.split()
        if p and p[0] == "EV" and int(p[3]) < 4:
            spots.append((int(p[1]), int(p[2]), int(p[3])))
    start = struct.unpack("<IIIQ", open(os.path.join(ROOT, "states", f"slot{slot}.p2s"), "rb").read(20))[3]
    done, results = [], []
    out = open(os.path.join(ROOT, "build", "spots_" + os.path.basename(path)), "w")
    for i, (x, z, d) in enumerate(spots):
        if i < first or i >= last:
            continue
        if any(max(abs(x - a), abs(z - b)) < spread for a, b in done):
            continue
        done.append((x, z))
        env = dict(os.environ, P2_NO_ENCOUNTERS="1", P2_TOUR=f"{path},{i},{i + 1}", P2_TOUR_SPREAD="0", P2_TOUR_AREA_SLOT=str(950 + len(done)))
        r = subprocess.run([EXE, "--headless", "--load-state", slot, "--turbo-until", "99999999", "--exit-after", str(start + 9000)],
                           cwd=ROOT, capture_output=True, text=True, errors="replace", env=env)
        log = r.stdout + r.stderr
        area = re.search(r"AREA CHANGE at vblank (\d+)", log)
        dialogue = "DIALOGUE" in log
        timeout = "timed out" in log
        no_route = "no known route" in log
        outcome = "AREA(slot %d)" % (950 + len(done)) if area else ("DIALOGUE" if dialogue else ("TIMEOUT" if timeout else ("NOROUTE" if no_route else "nothing")))
        line = f"spot #{i} cell ({x},{z}) dir {d}: {outcome}"
        print(line, flush=True)
        out.write(line + "\n")
        out.flush()


if __name__ == "__main__":
    main()
