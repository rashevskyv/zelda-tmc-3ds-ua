#!/usr/bin/env python3
"""Render a /room blob from the game's stereo link to PNGs (bottom, top, both) to check the format."""
import struct, sys
from PIL import Image

def parse(data):
    assert data[:4] == b'TMCR'
    ver, area, room = struct.unpack_from('<HBB', data, 4)
    w, h, ox, oy, bgb, bgt, ts, cols, rows, sx, sy, nent = struct.unpack_from('<12H', data, 8)
    off = 32
    layers = []
    for _ in range(2):
        mp = struct.unpack_from('<4096H', data, off); off += 8192
        col = data[off:off+4096]; off += 4096
        act = data[off:off+4096]; off += 4096
        sub = struct.unpack_from('<8192H', data, off); off += 16384
        layers.append(dict(map=mp, col=col, act=act, sub=sub))
    vram = data[off:off+0x10000]; off += 0x10000
    pal = struct.unpack_from('<256H', data, off); off += 512
    return dict(area=area, room=room, w=w, h=h, bg=(bgb, bgt), layers=layers, vram=vram, pal=pal, off=off)

def rgb(c):
    return ((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3)

def render(r, li):
    bgcnt = r['bg'][li]
    img = Image.new('RGBA', (r['w'], r['h']), (0, 0, 0, 0))
    if not bgcnt:
        return img
    base = ((bgcnt >> 2) & 3) * 0x4000
    L = r['layers'][li]; vram = r['vram']; pal = r['pal']
    px = img.load()
    for ty in range(r['h'] // 16):
        for tx in range(r['w'] // 16):
            idx = L['map'][tx | (ty << 6)] & 2047
            for q in range(4):
                e = L['sub'][idx * 4 + q]
                tile = e & 0x3ff; hf = e >> 10 & 1; vf = e >> 11 & 1; pb = (e >> 12) * 16
                a = (base + tile * 32) & 0xffff
                for y in range(8):
                    for x in range(8):
                        sx_ = 7 - x if hf else x; sy_ = 7 - y if vf else y
                        b = vram[(a + sy_ * 4 + sx_ // 2) & 0xffff]
                        ci = (b >> 4) if sx_ & 1 else (b & 15)
                        if ci:
                            px[tx * 16 + (q & 1) * 8 + x, ty * 16 + (q >> 1) * 8 + y] = rgb(pal[pb + ci]) + (255,)
    return img

r = parse(open(sys.argv[1], 'rb').read())
bottom, top = render(r, 0), render(r, 1)
both = Image.new('RGBA', bottom.size, rgb(r['pal'][0]) + (255,))
both.alpha_composite(bottom); both.alpha_composite(top)
out = sys.argv[2] if len(sys.argv) > 2 else 'room'
both.save(out + '.png'); bottom.save(out + '-bottom.png'); top.save(out + '-top.png')
print(r['area'], r['room'], r['w'], r['h'], 'bg', [hex(b) for b in r['bg']])
