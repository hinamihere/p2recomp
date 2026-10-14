"""Contact sheet of the frames in build/frames (dumped with P2_DUMP_VBLANK=1) nearest to the given vblanks.

usage: framesheet.py <out.png> <vblank> [vblank ...]
"""
import os
import sys

from PIL import Image

frames = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "frames")
names = sorted(os.listdir(frames))
times = [int(n.split("_")[1].split(".")[0]) for n in names]
picks = []
for v in map(int, sys.argv[2:]):
    best = min(range(len(times)), key=lambda i: abs(times[i] - v))
    picks.append(names[best])
cols = min(4, len(picks))
rows = (len(picks) + cols - 1) // cols
sheet = Image.new("RGB", (480 * cols, 272 * rows))
for i, n in enumerate(picks):
    sheet.paste(Image.open(os.path.join(frames, n)), ((i % cols) * 480, (i // cols) * 272))
sheet.save(sys.argv[1])
print(picks)
