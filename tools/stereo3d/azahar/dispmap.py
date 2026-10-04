#!/usr/bin/env python3
"""Print a per-cell disparity map (8x8 GBA pixels per cell) from a grab.png.

    python3 dispmap.py grab.png

Each digit is the disparity that best matches that cell between the eyes
('.' where the cell is too flat to tell). Cells are aligned to the left eye.
"""
import sys
import numpy as np
from PIL import Image
LEFT, RIGHT = (190, 57, 567, 509), (931, 57, 1308, 509)
im = Image.open(sys.argv[1]).convert("RGB")
L = np.asarray(im.crop(LEFT).resize((240, 160), Image.NEAREST)).astype(int)
R = np.asarray(im.crop(RIGHT).resize((240, 160), Image.NEAREST)).astype(int)
for cy in range(20):
    line = ""
    for cx in range(28):
        a = L[cy * 8:cy * 8 + 8, cx * 8:cx * 8 + 8]
        if a.std() < 6:
            line += "."
            continue
        res = sorted((abs(a - R[cy * 8:cy * 8 + 8, cx * 8 + s:cx * 8 + 8 + s]).mean(), s) for s in range(0, 12))
        line += "%x" % res[0][1] if res[0][0] * 1.5 < res[1][0] or res[0][0] < 2 else "?"
    print(line)
