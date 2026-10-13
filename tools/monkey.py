"""Generate a --input script: the scripted intro up to free control, then random play.

usage: monkey.py <seed> <end_vblank>
"""
import random
import sys

seed, end = int(sys.argv[1]), int(sys.argv[2])
rng = random.Random(seed)
parts = ["1650=START:6", "2000=UP:6", "2100=CROSS:6", "2300=CROSS:6"]
parts += [f"{t}=CROSS:4" for t in range(2500, 8600, 40)]
parts += ["8800=START:6", "9000=CROSS:6", "9200=CROSS:6", "9400=START:6", "9600=CROSS:6"]
parts += [f"{t}=CROSS:4" for t in range(9800, 11300, 40)]
t = 11400
dirs = ["UP", "DOWN", "LEFT", "RIGHT", "UP+LEFT", "UP+RIGHT", "DOWN+LEFT", "DOWN+RIGHT"]
while t < end:
    r = rng.random()
    if r < 0.55:
        parts.append(f"{t}={rng.choice(dirs)}:{rng.randint(20, 120)}")
        t += rng.randint(30, 130)
    elif r < 0.85:
        parts.append(f"{t}=CROSS:4")
        t += rng.randint(20, 60)
    elif r < 0.93:
        parts.append(f"{t}=CIRCLE:4")
        t += rng.randint(20, 60)
    else:
        parts.append(f"{t}={rng.choice(['TRIANGLE', 'SQUARE', 'START', 'L', 'R'])}:4")
        t += rng.randint(40, 120)
print(",".join(parts))
