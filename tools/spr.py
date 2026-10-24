"""Half-Life .spr (IDSP v2) reader -> list of RGBA PIL images"""
import struct
from PIL import Image

def read_spr(path):
    d = open(path, 'rb').read()
    magic, ver, typ, texfmt, rad, w, h, nframes, beam, sync = struct.unpack('<4siiifiiifi', d[:40])
    assert magic == b'IDSP' and ver == 2
    o = 40
    npal = struct.unpack('<h', d[o:o+2])[0]; o += 2
    pal = d[o:o+npal*3]; o += npal*3
    frames = []
    for _ in range(nframes):
        group = struct.unpack('<i', d[o:o+4])[0]; o += 4
        assert group == 0
        ox, oy, fw, fh = struct.unpack('<iiii', d[o:o+16]); o += 16
        px = d[o:o+fw*fh]; o += fw*fh
        im = Image.new('RGBA', (fw, fh))
        out = []
        for p in px:
            r, g, b = pal[p*3:p*3+3]
            if texfmt == 3 and p == 255: out.append((0, 0, 0, 0))     # SPR_ALPHTEST
            elif texfmt == 2: out.append((r, g, b, p))                 # SPR_INDEXALPHA
            else: out.append((r, g, b, 255))
        im.putdata(out)
        frames.append(im)
    return texfmt, frames
