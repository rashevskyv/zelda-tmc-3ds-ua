#!/usr/bin/env python3
"""Disparity of regions in a /frame capture: python3 disparity.py frame.png name:x0,x1,y0,y1 ...
Positive = right eye moved right of the left one (behind the screen)."""
import sys
import numpy as np
from PIL import Image
im = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(int)
L, R = im[:, :240], im[:, 244:484]
for spec in sys.argv[2:]:
    name, box = spec.split(':'); x0, x1, y0, y1 = map(int, box.split(','))
    res = sorted((abs(L[y0:y1, x0:x1] - R[y0:y1, x0 + s:x1 + s]).mean(), s) for s in range(-8, 12) if x0 + s >= 0 and x1 + s <= 240)
    print(f'{name}: {res[0][1]} px (err {res[0][0]:.1f}, next {res[1][1]} at {res[1][0]:.1f})')
