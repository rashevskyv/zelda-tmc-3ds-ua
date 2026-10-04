#!/usr/bin/env python3
"""Compare the two eyes of an Azahar side-by-side window grab (grab.ps1).

    python3 eyes.py grab.png [name:x0,x1,y0,y1 ...]

Resamples each eye's top screen to 240x160 GBA pixels, writes <grab>-eyes.png
(left | right, 3x) and prints the horizontal disparity of each named region.
The crop boxes match a 1499x998 Azahar window with the default layout and the
game's STRETCH aspect; adjust LEFT/RIGHT for another window size.
"""
import sys
import numpy as np
from PIL import Image

LEFT, RIGHT = (190, 57, 567, 509), (931, 57, 1308, 509)

im = Image.open(sys.argv[1]).convert("RGB")
L = np.asarray(im.crop(LEFT).resize((240, 160), Image.NEAREST)).astype(int)
R = np.asarray(im.crop(RIGHT).resize((240, 160), Image.NEAREST)).astype(int)
print("eyes identical" if (L == R).all() else "eyes differ")
for spec in sys.argv[2:]:
    name, box = spec.split(":")
    x0, x1, y0, y1 = map(int, box.split(","))
    res = sorted((abs(L[y0:y1, x0:x1] - R[y0:y1, x0 + s:x1 + s]).mean(), s) for s in range(0, 14))
    print(f"{name}: disparity {res[0][1]} px (err {res[0][0]:.1f}; next {res[1][1]} at {res[1][0]:.1f})")
gap = np.zeros((160, 6, 3), int)
out = sys.argv[1].rsplit(".", 1)[0] + "-eyes.png"
Image.fromarray(np.concatenate([L, gap, R], axis=1).astype("uint8")).resize((486 * 3, 480), Image.NEAREST).save(out)
print("wrote", out)
