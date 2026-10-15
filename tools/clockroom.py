"""Smash the emblem clock of a 0005.smd classroom (Seven Sisters High) from a savestate taken just inside
door 1, i.e. right after the room loaded (the game ignores input for the first ~300 vblanks).

usage: clockroom.py <door-entry slot> <out slot> [extra vblanks]

Route (room axes: DOWN+LEFT = -x, DOWN+RIGHT = -z, UP+LEFT = +z, UP+RIGHT = +x): step off the entry line past the
dais, slide -x below the teacher's desk, push +z to the blackboard wall left of the desk, walk +x along the wall into
the MONMON trigger quad (x 536..605, z -60..-20; Yukino at x 667 stops the walk inside it), face the wall, press Cross.
The clock scene plus Yukino's line take about 350 vblanks; the result state is saved to the out slot.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECIPE = "300=DOWN+RIGHT:6,310=DOWN+LEFT:55,370=UP+LEFT:35,410=UP+RIGHT:14,429=UP+LEFT:3,437=CROSS:4"


def main():
    src, dst = sys.argv[1], sys.argv[2]
    extra = int(sys.argv[3]) if len(sys.argv) > 3 else 350
    cmd = [sys.executable, os.path.join(ROOT, "tools", "drive.py"), src, dst, str(437 + 12 + extra), RECIPE, "--sheet", "6"]
    sys.exit(subprocess.call(cmd, cwd=ROOT))


if __name__ == "__main__":
    main()
