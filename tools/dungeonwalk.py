"""Long random walk through a dungeon with periodic Cross presses (so battles are fought with the default
commands and dialogue is paged), from a savestate.

usage: dungeonwalk.py <from-slot> <to-slot> <seed> <vblanks> [sheet-frames]
Writes the contact sheet to build/drive/slot<to>_sheet.png (see drive.py).
"""
import os
import random
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def script(seed, length):
    rng = random.Random(seed)
    dirs = ["UP", "DOWN", "LEFT", "RIGHT", "UP+LEFT", "UP+RIGHT", "DOWN+LEFT", "DOWN+RIGHT"]
    events, t, last_cross = [], 10, 0
    while t < length - 60:
        d = rng.randint(30, 160)
        events.append((t, f"{rng.choice(dirs)}:{d}"))
        t += d + 2
        # a few Cross presses after each move: talks to / interacts with what is in front, fights, pages text
        for _ in range(rng.randint(1, 3)):
            events.append((t, "CROSS:4"))
            t += 22
    return ",".join(f"{when}={what}" for when, what in events)


def main():
    src, dst, seed, length = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    sheet = sys.argv[5] if len(sys.argv) > 5 else "16"
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "drive.py"), src, dst, str(length), script(seed, length), "--sheet", sheet],
                   cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
