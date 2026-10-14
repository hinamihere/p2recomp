"""True if a frame shows the Shopping Mall destination menu (red title bar + 'Please select')."""
import sys
import numpy as np
from PIL import Image


def is_mall(path):
    a = np.asarray(Image.open(path).convert("RGB")).astype(int)
    bar = a[2:20, 100:400]
    red = (bar[..., 0] > 90) & (bar[..., 1] < 70) & (bar[..., 2] < 70)
    white = bar.min(axis=2) > 200
    grid = a[60:250, 10:390]
    green = ((grid[..., 1] > 150) & (grid[..., 0] < 200) & (grid[..., 2] < 140)).mean()
    return red.mean() > 0.4 and white.mean() > 0.03 and green > 0.01


if __name__ == "__main__":
    import os
    d = sys.argv[1]
    print(sum(is_mall(os.path.join(d, n)) for n in sorted(os.listdir(d))))
