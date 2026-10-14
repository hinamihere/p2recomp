"""True while a frame shows the Town Map button-hint strip (Show / Menu / Confirm / Run)."""
import numpy as np
from PIL import Image
import os
_T = None
def template():
    global _T
    if _T is None:
        p = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "drive", "slot430.png")
        _T = np.asarray(Image.open(p).convert("L")).astype(int)[2:20, 270:475] > 150
    return _T
def on_map(path):
    a = np.asarray(Image.open(path).convert("L")).astype(int)[2:20, 270:475] > 150
    t = template()
    return (a & t).sum() / max(t.sum(), 1) > 0.8 and a.sum() < 1.6 * t.sum()
