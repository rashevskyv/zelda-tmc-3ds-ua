#!/usr/bin/env python3
"""Per-cell relief map: how much nearer each 8x8 cell is than in a flat capture.

    python3 reliefmap.py flat.png relief.png

Both are grab.png captures of the same scene, one without relief. Each digit is
the cell's disparity gain in GBA pixels (shift in the left eye minus shift in
the right eye); '.' = unchanged, '?' = could not tell.
"""
import sys
import numpy as np
from PIL import Image
LEFT, RIGHT = (190, 57, 567, 509), (931, 57, 1308, 509)
def eyes(f):
    im = Image.open(f).convert("RGB")
    return [np.asarray(im.crop(b).resize((240, 160), Image.NEAREST)).astype(int) for b in (LEFT, RIGHT)]
A, B = eyes(sys.argv[1]), eyes(sys.argv[2])
def shift(flat, rel, cx, cy):
    x0, y0 = cx * 8, cy * 8
    a = rel[y0:y0 + 8, x0:x0 + 8]
    if a.std() < 5:
        return None
    best = sorted((abs(a - flat[y0:y0 + 8, x0 - s:x0 + 8 - s]).mean(), s) for s in range(-3, 4))
    return best[0][1] if best[0][0] < 6 and best[0][0] + 1 < best[1][0] else None
for cy in range(20):
    line = ""
    for cx in range(1, 29):
        l, r = shift(A[0], B[0], cx, cy), shift(A[1], B[1], cx, cy)
        line += "?" if l is None or r is None else ("." if l - r == 0 else "%d" % (l - r) if 0 < l - r < 10 else "!")
    print(line)
