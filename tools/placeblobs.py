"""For every place node of a town map: navigate there (P2_MAPNAV), press Cross and print which branch.bin blob (door selector) the game reads.
usage: placeblobs.py <town-map slot> <node,node,...>"""
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("P2_EXE", os.path.join(ROOT, "build", "dev", "p2.exe"))
slot, nodes = sys.argv[1], [int(n) for n in sys.argv[2].split(",")]
v0 = struct.unpack("<IIIQ", open(os.path.join(ROOT, "states", f"slot{slot}.p2s"), "rb").read(20))[3]
procs = []
for n in nodes:
    env = dict(os.environ, P2_MAPNAV=str(n), P2_IOTRACE="1", P2_NO_ENCOUNTERS="1")
    # navigation finishes within ~600 vblanks; press Cross every 40 vblanks afterwards
    inp = ",".join(f"{v0 + 500 + 40 * i}=CROSS:3" for i in range(12))
    procs.append((n, subprocess.Popen([EXE, "--headless", "--load-state", slot, "--turbo-until", "99999999", "--exit-after", str(v0 + 1200), "--input", inp],
                                      env=env, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")))
for n, proc in procs:
    text = proc.communicate()[0]
    m = re.search(r"arrived at node %d ([^\r\n]*)" % n, text)
    arrived = m.group(1)[-40:] if m else False
    blobs = sorted({(int(b.group(1)) - 220485632) // 2048 - 1 for b in re.finditer(r"iotrace\] read fd \d+ pos (\d+) size 800", text)
                    if 220485632 < int(b.group(1)) < 221100000})
    print(f"node {n}: arrived={arrived!r} blobs={blobs}")
