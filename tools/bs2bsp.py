#!/usr/bin/env python3
"""Convert a PlayStation 2 Half-Life map (.bs2, Gearbox's version 40 format) into a PC
Half-Life map (.bsp, version 30), so the rest of the asset pipeline (bsp2n64.py) can use it.

Used for the PS2 version's Head-To-Head maps (datacore2, stalkyard2, snark_pit2, ...), which
were never released for the PC.

    bs2bsp.py map.bs2 out.bsp [--wad halflife.wad ...]

A .bs2 file is a 4-byte uncompressed size followed by a zlib stream. The map inside keeps the
engine's in-memory structures: offsets in it are byte offsets inside the file (turned back into
indices here), vectors are padded to 4 floats, textures are resampled to powers of two with
their own (reordered, 21-bit) palettes. The lump layout and structure sizes are documented by
BS2PC (github.com/Triang3l/BS2PC, Triang3l), which this follows; the code is written for this
project.
"""
import math
import struct
import sys
import zlib

GBX_VERSION = 40
# Gearbox lump order
L_PLANES, L_NODES, L_LEAFS, L_EDGES, L_SURFEDGES, L_VERTEXES, L_HULL0, L_CLIPNODES, L_MODELS, \
    L_FACES, L_MARKSURFACES, L_VISIBILITY, L_LIGHTING, L_TEXTURES, L_ENTITIES, L_POLYGONS = range(16)
NLUMPS = 16

GBX_NODE = 0x40
GBX_LEAF = 0x40
GBX_PLANE = 0x14
GBX_FACE = 0x90
GBX_TEXTURE = 0x40
GBX_MODEL = 0x50
NONE = 0xFFFFFFFF


def load_bs2(path):
    raw = open(path, 'rb').read()
    size = struct.unpack_from('<I', raw, 0)[0]
    if size in (29, 30, GBX_VERSION):          # already uncompressed
        return raw
    data = zlib.decompress(raw[4:])
    if len(data) != size:
        raise ValueError('%s: size %d, expected %d' % (path, len(data), size))
    return data


# ---------------------------------------------------------------- palettes
def _gbx24_from_id(c):
    sh = (c < 0x40) + (c < 0x80)
    return (c - (1 if ((c << sh) & 0x8F) == 0x84 else 0)) & 0xFF


def _gbx21_from_id(c):
    return _gbx24_from_id(c) >> 1


def _id_from_gbx21(c):
    c7 = min(0x7F, c)
    c8 = ((c7 << 1) | (c7 >> 6)) & 0xFF
    return (c8 + (1 if _gbx21_from_id(c8) < c7 else 0)) & 0xFF


ID_FROM_21 = bytes(_id_from_gbx21(c) for c in range(256))


def palette_index(i):
    # the PS2 CLUT stores colours 0-7, 16-23, 8-15, 24-31, ... (GS CSM1 ordering)
    return (i & 0b11100111) | ((i & 8) << 1) | ((i & 16) >> 1)


def pc_palette(data, pal_off, name):
    """768-byte RGB palette, in the PC's colour order"""
    kind = name[:1]
    full = kind in ('!', '{')            # liquids and alpha-tested textures keep 8-bit colour
    xor = 0xFF if kind == '-' else 0     # random-tiled textures were inverted before conversion
    out = bytearray(768)
    for i in range(256):
        o = pal_off + 4 * palette_index(i)
        for k in range(3):
            c = data[o + k]
            out[3 * i + k] = c if full else ID_FROM_21[(c ^ xor) & 0xFF]
    if kind == '{':
        out[765:768] = b'\x00\x00\xff'   # the PC's transparent colour is blue
    return bytes(out)


def texture_pixels(data, t):
    """the base level at the scaled (power-of-two) size, deinterleaving random-tiled textures"""
    sw, sh, name = t['sw'], t['sh'], t['name']
    pix = data[t['pixels']:t['pixels'] + sw * sh]
    if name.startswith('-'):
        # rows of the two halves are interleaved, lower half first
        half = sh >> 1
        rows = [pix[y * sw:(y + 1) * sw] for y in range(sh)]
        out = []
        for y in range(sh):
            lower = y >= half
            src = ((y - (half if lower else 0)) << 1) | (0 if lower else 1)
            out.append(rows[src])
        pix = b''.join(out)
    return pix


def resample(pix, sw, sh, w, h):
    """nearest-neighbour from sw x sh to w x h"""
    xs = [x * sw // w for x in range(w)]
    out = bytearray(w * h)
    for y in range(h):
        row = pix[(y * sh // h) * sw:(y * sh // h + 1) * sw]
        o = y * w
        for x in range(w):
            out[o + x] = row[xs[x]]
    return bytes(out)


def miptex(name, w, h, pix, pal):
    """a PC miptex: header, 4 mip levels, palette"""
    mips = [pix]
    for m in range(1, 4):
        mw, mh = w >> m, h >> m
        src = pix
        mips.append(bytes(src[(y << m) * w + (x << m)] for y in range(mh) for x in range(mw)))
    hdr = 40
    offs = []
    o = hdr
    for m in mips:
        offs.append(o)
        o += len(m)
    nm = name.encode('latin1')[:15].ljust(16, b'\0')
    blob = nm + struct.pack('<6I', w, h, *offs) + b''.join(mips) + struct.pack('<H', 256) + pal + b'\0\0'
    return blob


# ---------------------------------------------------------------- conversion
def convert(data, pc_textures=None):
    """pc_textures: {lowercase name: (width, height)} of the PC WADs' textures; a PS2 texture with the
    same name and size is left out of the map (the PC original is used, and shared with the campaign)"""
    if struct.unpack_from('<I', data, 0)[0] != GBX_VERSION:
        raise ValueError('not a PS2 (version 40) map')
    offs = struct.unpack_from('<16I', data, 4)
    lens = struct.unpack_from('<16I', data, 4 + 64)
    cnts = struct.unpack_from('<16I', data, 4 + 128)

    def index(ptr, lump, size, what):
        rel = ptr - offs[lump]
        if rel < 0 or rel % size or rel // size >= cnts[lump]:
            raise ValueError('bad %s offset %d' % (what, ptr))
        return rel // size

    # planes: normal, dist, type (u8) -> type (u32)
    planes = bytearray()
    for i in range(cnts[L_PLANES]):
        nx, ny, nz, d, ty = struct.unpack_from('<4fB', data, offs[L_PLANES] + i * GBX_PLANE)
        planes += struct.pack('<4fi', nx, ny, nz, d, ty)

    # vertexes: vec4 -> vec3
    verts = bytearray()
    for i in range(cnts[L_VERTEXES]):
        verts += data[offs[L_VERTEXES] + i * 16:offs[L_VERTEXES] + i * 16 + 12]

    edges = data[offs[L_EDGES]:offs[L_EDGES] + 4 * cnts[L_EDGES]]
    surfedges = data[offs[L_SURFEDGES]:offs[L_SURFEDGES] + 4 * cnts[L_SURFEDGES]]
    clipnodes = data[offs[L_CLIPNODES]:offs[L_CLIPNODES] + 8 * cnts[L_CLIPNODES]]
    vis = data[offs[L_VISIBILITY]:offs[L_VISIBILITY] + lens[L_VISIBILITY]]

    # nodes
    nodes = bytearray()
    nn, nl = cnts[L_NODES], cnts[L_LEAFS]
    for i in range(nn):
        o = offs[L_NODES] + i * GBX_NODE
        _, _, _, plane = struct.unpack_from('<4I', data, o)
        mins = struct.unpack_from('<3f', data, o + 16)
        maxs = struct.unpack_from('<3f', data, o + 32)
        ch = struct.unpack_from('<2I', data, o + 48)
        ff, fc = struct.unpack_from('<2H', data, o + 56)
        kids = []
        for c in ch:
            if offs[L_NODES] <= c < offs[L_NODES] + nn * GBX_NODE:
                kids.append(index(c, L_NODES, GBX_NODE, 'node'))
            else:
                kids.append(-1 - index(c, L_LEAFS, GBX_LEAF, 'leaf'))
        nodes += struct.pack('<I2h3h3h2H', index(plane, L_PLANES, GBX_PLANE, 'plane'), kids[0], kids[1],
                             *[int(v) for v in mins], *[int(v) for v in maxs], ff, fc)

    # leafs
    leafs = bytearray()
    for i in range(nl):
        o = offs[L_LEAFS] + i * GBX_LEAF
        contents = struct.unpack_from('<i', data, o)[0]
        mins = struct.unpack_from('<3f', data, o + 16)
        maxs = struct.unpack_from('<3f', data, o + 32)
        visofs, fm, mc = struct.unpack_from('<3I', data, o + 48)
        amb = data[o + 60:o + 64]
        visofs = -1 if visofs == NONE else visofs - offs[L_VISIBILITY]
        leafs += struct.pack('<ii3h3h2H', contents, visofs, *[int(v) for v in mins], *[int(v) for v in maxs], fm, mc) + amb

    marks = bytearray()
    for i in range(cnts[L_MARKSURFACES]):
        marks += struct.pack('<H', struct.unpack_from('<I', data, offs[L_MARKSURFACES] + 4 * i)[0])

    # models
    models = bytearray()
    for i in range(cnts[L_MODELS]):
        o = offs[L_MODELS] + i * GBX_MODEL
        mins = struct.unpack_from('<3f', data, o)
        maxs = struct.unpack_from('<3f', data, o + 16)
        org = struct.unpack_from('<3f', data, o + 32)
        heads = struct.unpack_from('<4i', data, o + 48)
        vl, ff, fc = struct.unpack_from('<3I', data, o + 64)
        models += struct.pack('<9f4i3i', *mins, *maxs, *org, *heads, vl, ff, fc)

    # textures
    texs = []
    for i in range(cnts[L_TEXTURES]):
        o = offs[L_TEXTURES] + i * GBX_TEXTURE
        pixels, pal, w, h, sw, sh = struct.unpack_from('<2I4H', data, o)
        name = data[o + 16:o + 32].split(b'\0')[0].decode('latin1')
        texs.append(dict(pixels=pixels, pal=pal, w=w, h=h, sw=sw, sh=sh, name=name))
    tlump = bytearray(struct.pack('<I', len(texs)))
    blobs = []
    for t in texs:
        if pc_textures and pc_textures.get(t['name'].lower()) == (t['w'], t['h']):
            blobs.append(t['name'].encode('latin1')[:15].ljust(16, b'\0') + struct.pack('<6I', t['w'], t['h'], 0, 0, 0, 0))
            continue
        pix = texture_pixels(data, t)
        if (t['sw'], t['sh']) != (t['w'], t['h']):
            pix = resample(pix, t['sw'], t['sh'], t['w'], t['h'])
        blobs.append(miptex(t['name'], t['w'], t['h'], pix, pc_palette(data, t['pal'], t['name'])))
    o = 4 + 4 * len(blobs)
    for b in blobs:
        tlump += struct.pack('<i', o)
        o += len(b)
    for b in blobs:
        tlump += b

    # faces -> faces + texinfo
    texinfo = []
    tidx = {}
    faces = bytearray()
    newlight = bytearray()
    vtx = [struct.unpack_from('<3f', data, offs[L_VERTEXES] + 16 * i) for i in range(cnts[L_VERTEXES])]
    edg = [struct.unpack_from('<2H', data, offs[L_EDGES] + 4 * i) for i in range(cnts[L_EDGES])]
    sfe = struct.unpack_from('<%di' % cnts[L_SURFEDGES], data, offs[L_SURFEDGES])

    def relight(data, offs, rel, fo, sv, tv, fe, ec, styles):
        """the face's lightmaps, re-cut to the extents the PC tools work out from the texture axes
        (the PS2 stored its own, which differ by a luxel on a few faces)"""
        tm = struct.unpack_from('<2h', data, fo + 64)
        ex = struct.unpack_from('<2h', data, fo + 68)
        mins, maxs = [1e30, 1e30], [-1e30, -1e30]
        for k in range(ec):
            se = sfe[fe + k]
            v = vtx[edg[se][0]] if se >= 0 else vtx[edg[-se][1]]
            for a, vec in enumerate((sv, tv)):
                val = v[0] * vec[0] + v[1] * vec[1] + v[2] * vec[2] + vec[3]
                mins[a] = min(mins[a], val); maxs[a] = max(maxs[a], val)
        bmin = [math.floor(mins[a] / 16) for a in range(2)]
        bmax = [math.ceil(maxs[a] / 16) for a in range(2)]
        pw, ph = bmax[0] - bmin[0] + 1, bmax[1] - bmin[1] + 1
        sw, sh = ex[0] // 16 + 1, ex[1] // 16 + 1
        dx, dy = bmin[0] - tm[0] // 16, bmin[1] - tm[1] // 16
        nst = sum(1 for c in styles if c != 255)
        base = offs[L_LIGHTING] + rel
        out = bytearray()
        for st in range(nst):
            src = base + st * sw * sh * 3
            for y in range(ph):
                yy = min(max(y + dy, 0), sh - 1)
                for x in range(pw):
                    xx = min(max(x + dx, 0), sw - 1)
                    q = src + (yy * sw + xx) * 3
                    out += data[q:q + 3]
        return bytes(out)

    for i in range(cnts[L_FACES]):
        o = offs[L_FACES] + i * GBX_FACE
        sv = struct.unpack_from('<4f', data, o)
        tv = struct.unpack_from('<4f', data, o + 16)
        side, flags, tex, lofs, plane = struct.unpack_from('<2H3I', data, o + 32)
        fe, ec = struct.unpack_from('<2I', data, o + 52)
        styles = data[o + 0x64:o + 0x68]
        ti_tex = index(tex, L_TEXTURES, GBX_TEXTURE, 'texture')
        tname = texs[ti_tex]['name'].lower()
        special = 1 if (tname.startswith('sky') or tname.startswith('!') or tname.startswith('*')
                        or tname in ('aaatrigger', 'clip', 'origin', 'null')) else 0
        ti = struct.pack('<8f2I', *sv, *tv, ti_tex, special)
        if ti not in tidx:
            tidx[ti] = len(texinfo)
            texinfo.append(ti)
        if lofs == NONE:
            pofs = -1
        else:
            pofs = len(newlight)
            newlight += relight(data, offs, lofs - offs[L_LIGHTING], o, sv, tv, fe, ec, styles)
        faces += struct.pack('<HHIHH', index(plane, L_PLANES, GBX_PLANE, 'plane'), side, fe, ec, tidx[ti]) + styles + struct.pack('<i', pofs)

    # entities: PS2 model and sprite file names back to the PC's
    ent = data[offs[L_ENTITIES]:offs[L_ENTITIES] + lens[L_ENTITIES]].split(b'\0')[0]
    ent = ent.replace(b'.spz"', b'.spr"').replace(b'.dol"', b'.mdl"') + b'\0'

    # PC lump order: entities, planes, textures, vertexes, visibility, nodes, texinfo, faces,
    # lighting, clipnodes, leafs, marksurfaces, edges, surfedges, models
    lumps = [ent, bytes(planes), bytes(tlump), bytes(verts), vis, bytes(nodes), b''.join(texinfo), bytes(faces),
             bytes(newlight), clipnodes, bytes(leafs), bytes(marks), edges, surfedges, bytes(models)]
    out = bytearray(struct.pack('<i', 30) + b'\0' * (15 * 8))
    for k, l in enumerate(lumps):
        while len(out) % 4:
            out += b'\0'
        struct.pack_into('<ii', out, 4 + 8 * k, len(out), len(l))
        out += l
    return bytes(out)


def wad_sizes(paths):
    """{lowercase name: (width, height)} of the textures in WAD3 files"""
    out = {}
    for p in paths:
        try:
            d = open(p, 'rb').read()
        except OSError:
            continue
        if d[:4] != b'WAD3':
            continue
        n, ofs = struct.unpack_from('<ii', d, 4)
        for i in range(n):
            fp, dsz, sz, typ, comp, _, _, name = struct.unpack_from('<iiibbbb16s', d, ofs + 32 * i)
            if typ == 0x43:
                w, h = struct.unpack_from('<II', d, fp + 16)
                out.setdefault(name.split(b'\0')[0].decode('latin1').lower(), (w, h))
    return out


def main():
    args = sys.argv[1:]
    wads = []
    while '--wad' in args:
        i = args.index('--wad')
        wads.append(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__)
        sys.exit(1)
    open(args[1], 'wb').write(convert(load_bs2(args[0]), wad_sizes(wads) if wads else None))


if __name__ == '__main__':
    main()
