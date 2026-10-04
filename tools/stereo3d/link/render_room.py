#!/usr/bin/env python3
"""Render a /room blob from the game's stereo link the way the PC editor does:
both map layers (special tiles resolved), the OAM sprites by priority.
    render_room.py room.bin out          -> out.png, out-bottom.png, out-top.png, out-sprites.png
    render_room.py room.bin out --camera -> also out-camera.png, the console camera's 240x160"""
import struct, sys
from PIL import Image

def parse(d):
    assert d[:4] == b'TMCR'
    r = dict(area=d[6], room=d[7])
    (r['w'], r['h'], r['ox'], r['oy'], bgb, bgt, r['ts'], r['cols'], r['rows'], sx, sy, nent) = struct.unpack_from('<12H', d, 8)
    r['bg'] = (bgb, bgt); r['sx'] = struct.unpack_from('<h', d, 26)[0]; r['sy'] = struct.unpack_from('<h', d, 28)[0]
    off = 32; r['layers'] = []
    for _ in range(2):
        mp = struct.unpack_from('<4096H', d, off); off += 8192
        off += 8192  # collision, act
        sub = struct.unpack_from('<8192H', d, off); off += 16384
        r['layers'].append((mp, sub))
    r['vram'] = d[off:off + 0x10000]; off += 0x10000
    r['pal'] = struct.unpack_from('<256H', d, off); off += 512
    off += 4 * 16384 + 32768 + nent * 12
    if d[off:off + 4] == b'OAMS':
        r['dispcnt'] = struct.unpack_from('<H', d, off + 4)[0]; off += 8
        r['oam'] = struct.unpack_from('<512H', d, off); off += 0x400
        r['objvram'] = d[off:off + 0x8000]; off += 0x8000
        r['objpal'] = struct.unpack_from('<256H', d, off); off += 0x200
    r['shots'] = []
    while d[off:off + 4] == b'OAMX':
        sx_, sy_, dc = struct.unpack_from('<hhH', d, off + 4); off += 12
        oam = struct.unpack_from('<512H', d, off); off += 0x400
        r['shots'].append(dict(sx=sx_, sy=sy_, dispcnt=dc, oam=oam, objvram=d[off:off + 0x8000])); off += 0x8000
    if d[off:off + 4] == b'RIDX':
        off += 4; r['ridx'] = [struct.unpack_from('<4096H', d, off), struct.unpack_from('<4096H', d, off + 8192)]
    return r

def rgb(c): return ((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3)

def layer(r, L):
    img = Image.new('RGBA', (r['w'], r['h'])); bgcnt = r['bg'][L]
    if not bgcnt: return img
    px = img.load(); base = ((bgcnt >> 2) & 3) * 0x4000; mp, sub = r['layers'][L]; vram = r['vram']; pal = r['pal']
    for ty in range(r['h'] // 16):
        for tx in range(r['w'] // 16):
            t = tx | (ty << 6)
            idx = (r['ridx'][L][t] if 'ridx' in r else mp[t]) & 2047
            for q in range(4):
                e = sub[idx * 4 + q]; tile = e & 0x3ff; hf = e >> 10 & 1; vf = e >> 11 & 1; pb = (e >> 12) * 16
                a = base + tile * 32
                for y in range(8):
                    for x in range(8):
                        sx_ = 7 - x if hf else x; sy_ = 7 - y if vf else y
                        b = vram[(a + sy_ * 4 + sx_ // 2) & 0xffff]; ci = (b >> 4) if sx_ & 1 else (b & 15)
                        if ci: px[tx * 16 + (q & 1) * 8 + x, ty * 16 + (q >> 1) * 8 + y] = rgb(pal[pb + ci]) + (255,)
    return img

SIZES = [[(8, 8), (16, 16), (32, 32), (64, 64)], [(16, 8), (32, 8), (32, 16), (64, 32)], [(8, 16), (8, 32), (16, 32), (32, 64)]]

def sprites(r):
    out = [Image.new('RGBA', (r['w'], r['h'])) for _ in range(4)]
    if 'oam' not in r: return out
    pxs = [o.load() for o in out]
    for shot in r['shots'] + [dict(sx=r['sx'], sy=r['sy'], dispcnt=r['dispcnt'], oam=r['oam'], objvram=r['objvram'])]:
        draw_oam(r, shot, pxs)
    return out

def draw_oam(r, shot, pxs):
    oned = shot['dispcnt'] & 0x40; camx = shot['sx'] - r['ox']; camy = shot['sy'] - r['oy']
    for i in range(127, -1, -1):
        a0, a1, a2 = shot['oam'][i * 4:i * 4 + 3]
        aff = a0 & 0x100; dbl = a0 & 0x200
        if not aff and dbl: continue
        gfx = (a0 >> 10) & 3
        if gfx >= 2 or (a0 >> 14) == 3: continue
        w, h = SIZES[a0 >> 14][a1 >> 14]
        x = a1 & 511; x = x - 512 if x & 256 else x
        y = a0 & 255; y = y - 256 if y >= 160 else y
        if aff and dbl: x += w // 2; y += h // 2
        hf = not aff and a1 & 0x1000; vf = not aff and a1 & 0x2000; c256 = a0 & 0x2000
        tile = a2 & 0x3ff; prio = (a2 >> 10) & 3; bank = (a2 >> 12) * 16
        if prio == 0: continue
        step = 2 if c256 else 1
        for py in range(h):
            for px_ in range(w):
                sx_ = w - 1 - px_ if hf else px_; sy_ = h - 1 - py if vf else py
                t = tile + ((sy_ >> 3) * (w >> 3) + (sx_ >> 3)) * step if oned else tile + (sy_ >> 3) * 32 + (sx_ >> 3) * step
                if c256: ci = shot['objvram'][(t * 32 + (sy_ & 7) * 8 + (sx_ & 7)) & 0x7fff]
                else:
                    b = shot['objvram'][(t * 32 + (sy_ & 7) * 4 + ((sx_ & 7) >> 1)) & 0x7fff]; ci = b >> 4 if sx_ & 1 else b & 15
                if not ci: continue
                rx, ry = camx + x + px_, camy + y + py
                if 0 <= rx < r['w'] and 0 <= ry < r['h']:
                    pxs[prio][rx, ry] = rgb(r['objpal'][ci if c256 else bank + ci]) + (255,)

r = parse(open(sys.argv[1], 'rb').read()); out = sys.argv[2]
L = [layer(r, 0), layer(r, 1)]; S = sprites(r)
img = Image.new('RGBA', (r['w'], r['h']), rgb(r['pal'][0]) + (255,))
for p in (3, 2, 1, 0):
    if r['bg'][0] and r['bg'][0] & 3 == p: img.alpha_composite(L[0])
    if r['bg'][1] and r['bg'][1] & 3 == p: img.alpha_composite(L[1])
    img.alpha_composite(S[p])
img.save(out + '.png'); L[0].save(out + '-bottom.png'); L[1].save(out + '-top.png')
spr = Image.new('RGBA', (r['w'], r['h'])); [spr.alpha_composite(s) for s in S]; spr.save(out + '-sprites.png')
if '--camera' in sys.argv:
    cx, cy = r['sx'] - r['ox'], r['sy'] - r['oy']; img.crop((cx, cy, cx + 240, cy + 160)).save(out + '-camera.png')
print(hex(r['area']), hex(r['room']), r['w'], r['h'], 'bg', [hex(b) for b in r['bg']], 'camera', r['sx'] - r['ox'], r['sy'] - r['oy'])
