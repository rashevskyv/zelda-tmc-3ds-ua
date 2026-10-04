#!/usr/bin/env python3
"""Fetch GET /bottom (the panel image of the bottom screen) and save it as PNG."""
import struct, sys, urllib.request
from PIL import Image
d = urllib.request.urlopen(f'http://{sys.argv[1]}:8333/bottom', timeout=10).read()
assert d[:4] == b'TMCB', d[:60]
w, h = struct.unpack_from('<HH', d, 4)
px = struct.unpack_from(f'<{w*h}I', d, 8)
im = Image.new('RGB', (w, h)); im.putdata([(p & 255, (p >> 8) & 255, (p >> 16) & 255) for p in px])
im.save(sys.argv[2] if len(sys.argv) > 2 else 'bottom.png')
