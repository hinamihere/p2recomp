"""Play the game in steps from savestates: load a slot, run scripted input, save another slot.

usage: drive.py <from-slot|boot> <to-slot> <vblanks> ["<rel-vblank>=BUTTON[+BUTTON][:frames],..."] [--sheet N]

Input times are relative to the loaded state's vblank. The run is fast-forwarded except
for the last few vblanks; the final frame goes to build/drive/slot<to>.png and, with
--sheet N, N evenly spaced frames to build/drive/slot<to>_sheet.png.
"""
import os
import shutil
import struct
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "release", "p2.exe")
if not os.path.exists(EXE):
    EXE = os.path.join(ROOT, "build", "ascii", "p2.exe")


def state_vblank(slot):
    with open(os.path.join(ROOT, "states", f"slot{slot}.p2s"), "rb") as f:
        magic, version, _gp, vblank = struct.unpack("<IIIQ", f.read(20))
    if magic != 0x53533250 or version < 2:
        sys.exit(f"slot {slot}: not a v2+ savestate")
    return vblank


def main():
    args = sys.argv[1:]
    sheet = 0
    if "--sheet" in args:
        i = args.index("--sheet")
        sheet = int(args[i + 1])
        del args[i:i + 2]
    src, dst, length = args[0], int(args[1]), int(args[2])
    script = args[3] if len(args) > 3 else ""
    if src != "boot" and not os.path.exists(os.path.join(ROOT, "states", f"slot{src}.p2s")):
        sys.exit(f"no state in slot {src}")
    start = 0 if src == "boot" else state_vblank(int(src))
    end = start + length

    parts = []
    for item in filter(None, script.split(",")):
        t, rest = item.split("=", 1)
        parts.append(f"{start + int(t)}={rest}")

    frames = os.path.join(ROOT, "build", "frames")
    shutil.rmtree(frames, ignore_errors=True)
    # The save may be postponed a little (interrupt in flight): run on until it is written.
    cmd = [EXE, "--headless", "-q", "--exit-after", str(end + 240), "--exit-after-save", "--save-state-at", f"{end - 2}:{dst}",
           "--turbo-until", str(max(end - 24, start)), "--dump-frames", "1"]
    if src != "boot":
        cmd += ["--load-state", src]
    if parts:
        cmd += ["--input", ",".join(parts)]
    target = os.path.join(ROOT, "states", f"slot{dst}.p2s")
    before = os.path.getmtime(target) if os.path.exists(target) else 0
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace")
    for line in (proc.stdout + proc.stderr).splitlines():
        if "[E" in line or "state" in line:
            print(line)

    names = sorted(os.listdir(frames)) if os.path.isdir(frames) else []
    out = os.path.join(ROOT, "build", "drive")
    os.makedirs(out, exist_ok=True)
    if names:
        Image.open(os.path.join(frames, names[-1])).save(os.path.join(out, f"slot{dst}.png"))
    if sheet and names:
        # Only frames after the load (presented counter restarts at boot).
        picks = [names[int(i * (len(names) - 1) / max(sheet - 1, 1))] for i in range(sheet)]
        cols = min(sheet, 4)
        rows = (sheet + cols - 1) // cols
        img = Image.new("RGB", (480 * cols, 272 * rows))
        for i, n in enumerate(picks):
            img.paste(Image.open(os.path.join(frames, n)), ((i % cols) * 480, (i // cols) * 272))
        img.save(os.path.join(out, f"slot{dst}_sheet.png"))
    if not os.path.exists(target) or os.path.getmtime(target) == before:
        sys.exit(f"vblank {start} -> {end}: slot {dst} was NOT written (see game log)")
    print(f"vblank {start} -> {end}, {len(names)} frames, slot {dst}")


if __name__ == "__main__":
    main()
