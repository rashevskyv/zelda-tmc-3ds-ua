#!/usr/bin/env python3
"""Fetch GET /frame from the game's stereo link and save left|right as one PNG."""
import struct, sys, urllib.request
from PIL import Image
host = sys.argv[1]; out = sys.argv[2] if len(sys.argv) > 2 else 'frame.png'
d = urllib.request.urlopen(f'http://{host}:8333/frame', timeout=10).read()
assert d[:4] == b'TMCF', d[:80]
w, h = struct.unpack_from('<HH', d, 4)
img = Image.new('RGB', (w * 2 + 4, h))
for eye in range(2):
    px = struct.unpack_from(f'<{w*h}H', d, 8 + eye * w * h * 2)
    im = Image.new('RGB', (w, h))
    # RGBA5551: r in the top five bits
    im.putdata([(((p >> 11) & 31) << 3, ((p >> 6) & 31) << 3, ((p >> 1) & 31) << 3) for p in px])
    img.paste(im, (eye * (w + 4), 0))
img.save(out)
print('saved', out, w, h)
