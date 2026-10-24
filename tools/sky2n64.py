#!/usr/bin/env python3
"""
sky2n64.py - convert a GoldSrc skybox (gfx/env/<name>{rt,lf,bk,ft,up,dn}.tga) for HL64.
Each face becomes 64x64 texels, quantised to its own 256-colour palette and split into
four 32x32 CI8 tiles (1 KB each, so each tile + palette fits in the N64's 4 KB TMEM).
Output: 'SKY1' + 6 x (512-byte RGBA5551 palette + 4 x 1024-byte tiles), face order rt lf bk ft up dn.
"""
import sys, os, struct
from PIL import Image
import numpy as np

FACES = ['rt', 'lf', 'bk', 'ft', 'up', 'dn']


def convert(envdir, name, out):
    blob = bytearray(b'SKY1')
    for f in FACES:
        path = os.path.join(envdir, name + f + '.tga')
        img = Image.open(path).convert('RGB').resize((64, 64), Image.LANCZOS)
        q = img.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        pal = np.array(q.getpalette()[:768] + [0] * (768 - len(q.getpalette()[:768])), np.uint16).reshape(256, 3)
        p16 = ((pal[:, 0] >> 3) << 11) | ((pal[:, 1] >> 3) << 6) | ((pal[:, 2] >> 3) << 1) | 1
        blob += p16.astype('>u2').tobytes()
        idx = np.array(q, np.uint8)
        for ty in range(2):
            for tx in range(2):
                blob += idx[ty * 32:(ty + 1) * 32, tx * 32:(tx + 1) * 32].tobytes()
    open(out, 'wb').write(blob)


if __name__ == '__main__':
    convert(sys.argv[1], sys.argv[2], sys.argv[3])
