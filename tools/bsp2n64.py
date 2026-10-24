#!/usr/bin/env python3
"""
bsp2n64.py - Convert a Half-Life (GoldSrc, BSP v30) map into the HL64 runtime
format used by the Nintendo 64 engine in this project.

What it does
  * Geometry: every face is subdivided on its lightmap grid and triangulated;
    the lightmap is baked into per-vertex colour (the N64 has 4 KB of TMEM,
    there is no room for a second lightmap texture per surface).
  * Textures: the original 8-bit palettised textures are kept as CI8 (the
    N64 supports 8-bit colour-indexed textures natively) and resampled to
    power-of-two sizes that fit in TMEM (<= 2048 texels with the palette).
  * Collision: planes + clipnodes for all four GoldSrc hulls are kept exactly,
    hull 0 is generated from the BSP nodes the way the engine does it, so
    Valve's pm_shared.c player movement collides exactly like on PC.
  * Visibility: leaf PVS data is kept (run-length compressed, as on PC).
  * Entities: the entity lump is copied verbatim and parsed on the N64.

Output is big-endian (the VR4300 runs big-endian).

Usage: bsp2n64.py map.bsp out.hlb [--wad path.wad ...] [--grid 64] [--maxtexels 2048]
"""
import struct, sys, os, math, argparse
import numpy as np

LUMP_NAMES = ['ENTITIES', 'PLANES', 'TEXTURES', 'VERTICES', 'VISIBILITY', 'NODES',
              'TEXINFO', 'FACES', 'LIGHTING', 'CLIPNODES', 'LEAVES', 'MARKSURFACES',
              'EDGES', 'SURFEDGES', 'MODELS']
L = {n: i for i, n in enumerate(LUMP_NAMES)}

# face flags (runtime)
FF_SKY, FF_NODRAW, FF_ALPHATEST, FF_WATER, FF_TRANS = 1, 2, 4, 8, 16

NODRAW_TEXTURES = {'aaatrigger', 'clip', 'null', 'origin', 'hint', 'skip', 'bevel',
                   'black_hidden', 'noclip'}

E = '>'  # output byte order: N64 is big-endian ('<' only for the PC test harness)
VERT_SCALE = 4  # int16 vertex positions are world units * 4 (0.25 unit precision)
MAX_CHUNK_VERTS = 64  # tiny3d vertex cache is 70


class BSP:
    def __init__(self, data):
        self.d = data
        ver = struct.unpack_from('<i', data, 0)[0]
        if ver != 30:
            raise SystemExit('Not a Half-Life BSP (version %d)' % ver)
        self.lumps = [struct.unpack_from('<ii', data, 4 + 8 * i) for i in range(15)]

    def raw(self, name):
        o, l = self.lumps[L[name]]
        return self.d[o:o + l]

    def arr(self, name, fmt):
        b = self.raw(name)
        sz = struct.calcsize(fmt)
        return [struct.unpack_from(fmt, b, i * sz) for i in range(len(b) // sz)]


# ---------------------------------------------------------------- textures

def read_wads(paths):
    tex = {}
    for p in paths:
        try:
            d = open(p, 'rb').read()
        except OSError:
            continue
        if d[:4] not in (b'WAD3', b'WAD2'):
            continue
        n, ofs = struct.unpack_from('<ii', d, 4)
        for i in range(n):
            fp, dsz, sz, typ, comp, _, _, name = struct.unpack_from('<iiibbbb16s', d, ofs + 32 * i)
            if typ != 0x43:
                continue
            nm = name.split(b'\0')[0].decode('latin1').lower()
            tex.setdefault(nm, d[fp:fp + dsz])
    return tex


def parse_miptex(blob):
    name, w, h, o0, o1, o2, o3 = struct.unpack_from('<16sIIIIII', blob, 0)
    name = name.split(b'\0')[0].decode('latin1')
    if o0 == 0:
        return name, w, h, None, None
    pix = np.frombuffer(blob, np.uint8, w * h, o0).reshape(h, w)
    po = o3 + (w // 8) * (h // 8)
    ncol = struct.unpack_from('<H', blob, po)[0]
    pal = np.frombuffer(blob, np.uint8, 768, po + 2).reshape(256, 3)
    return name, w, h, pix, pal


def pow2_floor(x):
    p = 1
    while p * 2 <= x:
        p *= 2
    return p


def pow2_near(x):
    lo = pow2_floor(x)
    return lo * 2 if (x - lo) > (lo * 2 - x) else lo


def convert_texture(name, w, h, pix, pal, maxtexels):
    """Resample to power-of-two size fitting TMEM, re-index into original palette."""
    alpha = name.startswith('{')
    nw, nh = max(8, pow2_near(w)), max(4, pow2_near(h))
    while nw * nh > maxtexels:
        if nw >= nh:
            nw //= 2
        else:
            nh //= 2
    rgb = pal[pix].astype(np.float32)  # h,w,3
    mask = (pix == 255) if alpha else np.zeros(pix.shape, bool)
    # area-average resample (separable box filter via index ranges)
    out = np.zeros((nh, nw, 3), np.float32)
    outa = np.zeros((nh, nw), np.float32)
    ys = np.linspace(0, h, nh + 1)
    xs = np.linspace(0, w, nw + 1)
    for j in range(nh):
        y0, y1 = int(ys[j]), max(int(ys[j]) + 1, int(math.ceil(ys[j + 1])))
        for i in range(nw):
            x0, x1 = int(xs[i]), max(int(xs[i]) + 1, int(math.ceil(xs[i + 1])))
            blk = rgb[y0:y1, x0:x1].reshape(-1, 3)
            m = ~mask[y0:y1, x0:x1].reshape(-1)
            outa[j, i] = m.mean()
            if m.any():
                out[j, i] = blk[m].mean(0)
    # map to nearest palette entry (exclude index 255 for alpha textures)
    palf = pal.astype(np.float32)
    usable = 255 if alpha else 256
    flat = out.reshape(-1, 3)
    d = ((flat[:, None, :] - palf[None, :usable, :]) ** 2).sum(-1)
    idx = d.argmin(1).astype(np.uint8).reshape(nh, nw)
    if alpha:
        idx[outa < 0.5] = 255
    # palette -> RGBA5551 big-endian
    p16 = ((pal[:, 0].astype(np.uint16) >> 3) << 11) | ((pal[:, 1].astype(np.uint16) >> 3) << 6) | \
          ((pal[:, 2].astype(np.uint16) >> 3) << 1) | 1
    if alpha:
        p16[255] = 0
    return nw, nh, idx.tobytes(), p16.astype(E + 'u2').tobytes()


# ---------------------------------------------------------------- entities

def parse_entities(text):
    ents, cur = [], None
    i, n = 0, len(text)
    toks = []
    while i < n:
        c = text[i]
        if c in '{}':
            toks.append(c); i += 1
        elif c == '"':
            j = text.index('"', i + 1); toks.append(text[i + 1:j]); i = j + 1
        else:
            i += 1
    k = 0
    while k < len(toks):
        t = toks[k]
        if t == '{':
            cur = {}
        elif t == '}':
            ents.append(cur); cur = None
        else:
            cur[t] = toks[k + 1]; k += 1
        k += 1
    return ents


# ---------------------------------------------------------------- lighting

def gamma_table():
    # GoldSrc applies lightgamma 2.5 then brightness; approximate final curve.
    t = []
    for v in range(1024):
        x = min(v, 1023) / 255.0
        y = (x ** (1.0 / 2.2)) * 1.0 if x <= 1 else 1.0 + (x - 1) * 0.3
        t.append(min(255, int(y * 235 + 0.5)))
    return t


GAMMA = gamma_table()


# ---------------------------------------------------------------- polygon utils

def split_poly(poly, f, c):
    """poly: list of (pos(np3), s, t). split by f(v)-c. returns (back(<c), front(>=c))."""
    back, front = [], []
    n = len(poly)
    vals = [f(v) - c for v in poly]
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        da, db = vals[i], vals[(i + 1) % n]
        if da <= 0:
            back.append(a)
        if da >= 0:
            front.append(a)
        if (da < 0 < db) or (db < 0 < da):
            k = da / (da - db)
            p = a[0] + (b[0] - a[0]) * k
            v = (p, a[1] + (b[1] - a[1]) * k, a[2] + (b[2] - a[2]) * k)
            back.append(v); front.append(v)
    def clean(pl):
        out = []
        for v in pl:
            if not out or np.abs(out[-1][0] - v[0]).max() > 1e-4:
                out.append(v)
        if len(out) > 1 and np.abs(out[0][0] - out[-1][0]).max() <= 1e-4:
            out.pop()
        return out if len(out) >= 3 else []
    return clean(back), clean(front)


def subdivide(poly, grid):
    pieces = [poly]
    for axis in (1, 2):
        nxt = []
        for p in pieces:
            work = [p]
            vals = [v[axis] for v in p]
            lo, hi = min(vals), max(vals)
            k = math.floor(lo / grid) + 1
            while k * grid < hi - 0.01:
                c = k * grid
                if c > lo + 0.01:
                    cur = work.pop()
                    b, f = split_poly(cur, lambda v, a=axis: v[a], c)
                    if b:
                        work.append(b)
                    if f:
                        work.append(f)
                    if not f:
                        break
                k += 1
            nxt.extend(work)
        pieces = nxt
    return pieces


# ---------------------------------------------------------------- main conversion

GAMMA_NP = None


def choose_grid(lm, bmin, finest, tol):
    """Largest subdivision grid (texels) whose bilinear interpolation of the lightmap stays within
    `tol` (after gamma) of the real lightmap. Uniformly lit faces get few, large polygons."""
    global GAMMA_NP
    if GAMMA_NP is None:
        GAMMA_NP = np.array(GAMMA, np.float32)
    lum = lm.sum(0)                       # h,w,3  (all styles)
    lum = GAMMA_NP[np.clip(lum, 0, 1023).astype(np.int32)].max(-1)
    h, w = lum.shape
    if lum.max() - lum.min() <= tol:
        return 0                          # flat lighting: no subdivision at all
    ix, iy = np.arange(w), np.arange(h)
    for G in (512, 256, 128, 64, 32):
        if G < finest:
            break
        g = G // 16
        xs = sorted({0, w - 1} | {i for i in range(w) if (bmin[0] + i) % g == 0})
        ys = sorted({0, h - 1} | {j for j in range(h) if (bmin[1] + j) % g == 0})
        sub = lum[np.ix_(ys, xs)]
        rows = np.array([np.interp(ix, xs, sub[k]) for k in range(len(ys))])      # len(ys), w
        approx = np.array([np.interp(iy, ys, rows[:, c]) for c in range(w)]).T     # h, w
        if np.abs(approx - lum).max() <= tol:
            return G
    return finest


def convert(bsp_path, out_path, wads, grid, maxtexels, verbose=True, lm_tolerance=12):
    data = open(bsp_path, 'rb').read()
    bsp = BSP(data)

    ent_text = bsp.raw('ENTITIES').split(b'\0')[0].decode('latin1')
    ents = parse_entities(ent_text)

    planes = bsp.arr('PLANES', '<4fi')
    verts = [np.array(v, np.float64) for v in bsp.arr('VERTICES', '<3f')]
    nodes = bsp.arr('NODES', '<I2h3h3hHH')
    texinfo = bsp.arr('TEXINFO', '<8fii')
    faces = bsp.arr('FACES', '<HHiHH4Bi')
    clipnodes = bsp.arr('CLIPNODES', '<i2h')
    leafs = bsp.arr('LEAVES', '<ii3h3hHH4B')
    marks = bsp.arr('MARKSURFACES', '<H')
    edges = bsp.arr('EDGES', '<2H')
    surfedges = [x[0] for x in bsp.arr('SURFEDGES', '<i')]
    models = bsp.arr('MODELS', '<9f4i3i')
    lighting = bsp.raw('LIGHTING')
    vis = bsp.raw('VISIBILITY')

    # ---- textures
    traw = bsp.raw('TEXTURES')
    ntex = struct.unpack_from('<i', traw, 0)[0]
    toffs = struct.unpack_from('<%di' % ntex, traw, 4)
    wadtex = None
    textures = []
    for of in toffs:
        if of < 0:
            textures.append(None); continue
        name, w, h, pix, pal = parse_miptex(traw[of:])
        if pix is None:
            if wadtex is None:
                wadtex = read_wads(wads)
            blob = wadtex.get(name.lower())
            if blob is not None:
                _, w, h, pix, pal = parse_miptex(blob)
        lname = name.lower()
        flags = 0
        if lname.startswith('sky'):
            flags |= FF_SKY
        if lname in NODRAW_TEXTURES:
            flags |= FF_NODRAW
        if lname.startswith('{'):
            flags |= FF_ALPHATEST
        if lname.startswith('!') or lname.startswith('*'):
            flags |= FF_WATER
        if pix is None:
            # missing: magenta/black checker like the PC engine
            w0, h0 = max(16, w), max(16, h)
            pix = np.zeros((16, 16), np.uint8)
            pix[(np.arange(16)[:, None] // 8 + np.arange(16)[None, :] // 8) % 2 == 1] = 1
            pal = np.zeros((256, 3), np.uint8); pal[1] = (255, 0, 255)
            w, h = 16, 16
        if flags & (FF_SKY | FF_NODRAW):
            tw, th, tdat, tpal = 8, 4, bytes(32), bytes(512)
        else:
            tw, th, tdat, tpal = convert_texture(name, w, h, pix, pal, maxtexels)
        textures.append(dict(name=name, w=w, h=h, tw=tw, th=th, data=tdat, pal=tpal, flags=flags))

    # ---- lightstyles that are switched on at map start
    style_on = {0: 1.0}
    for s in range(1, 32):
        style_on[s] = 0.7  # animated presets: use average brightness
    for e in ents:
        if e.get('classname', '').startswith('light') and 'style' in e:
            try:
                st = int(e['style'])
            except ValueError:
                continue
            if st >= 32:
                off = int(e.get('spawnflags', '0') or 0) & 1
                style_on[st] = 0.0 if off else 1.0

    # ---- per-face geometry
    out_faces, out_chunks, out_verts, out_idx, out_facetex = [], [], [], [], []
    dyn_recs = []   # (vertex index, (base rgb, [(style, rgb)...]))
    face_jobs = {}
    face_grid_hist = {}
    face_tri_total = 0

    def lm_sample(lm, lw, lh, nstyles, ls, lt):
        ls = min(max(ls, 0.0), lw - 1.0)
        lt = min(max(lt, 0.0), lh - 1.0)
        x0, y0 = int(ls), int(lt)
        x1, y1 = min(x0 + 1, lw - 1), min(y0 + 1, lh - 1)
        fx, fy = ls - x0, lt - y0
        c = lm[:, y0, x0] * (1 - fx) * (1 - fy) + lm[:, y0, x1] * fx * (1 - fy) + \
            lm[:, y1, x0] * (1 - fx) * fy + lm[:, y1, x1] * fx * fy
        return c.sum(0)

    for fi, f in enumerate(faces):
        planenum, side, firstedge, numedges, tinum, s0, s1, s2, s3, lofs = f
        styles = [s for s in (s0, s1, s2, s3) if s != 255]
        ti = texinfo[tinum]
        sv = np.array(ti[0:3]); so = ti[3]
        tv = np.array(ti[4:7]); to = ti[7]
        miptex, tiflags = ti[8], ti[9]
        tex = textures[miptex] if 0 <= miptex < len(textures) and textures[miptex] else None
        flags = tex['flags'] if tex else FF_NODRAW
        poly = []
        for k in range(numedges):
            se = surfedges[firstedge + k]
            vi = edges[se][0] if se >= 0 else edges[-se][1]
            p = verts[vi]
            poly.append((p, float(np.dot(p, sv) + so), float(np.dot(p, tv) + to)))
        ss = [v[1] for v in poly]; ts = [v[2] for v in poly]
        bmin = (math.floor(min(ss) / 16), math.floor(min(ts) / 16))
        bmax = (math.ceil(max(ss) / 16), math.ceil(max(ts) / 16))
        ext = ((bmax[0] - bmin[0]) * 16, (bmax[1] - bmin[1]) * 16)
        out_facetex.append((tuple(ti[0:4]), tuple(ti[4:8]), bmin[0] * 16, bmin[1] * 16, ext[0], ext[1], miptex))

        if flags & (FF_SKY | FF_NODRAW) or numedges < 3:
            out_faces.append((planenum, side, flags, max(miptex, 0), len(out_chunks), 0))
            continue

        lw, lh = ext[0] // 16 + 1, ext[1] // 16 + 1
        lm = None
        if lofs >= 0 and styles and lighting:
            n = lw * lh * 3
            arrs = []
            for si, st in enumerate(styles):
                seg = np.frombuffer(lighting, np.uint8, n, lofs + si * n).astype(np.float32).reshape(lh, lw, 3)
                arrs.append(seg * style_on.get(st, 1.0))
            lm = np.stack(arrs)  # styles,h,w,3
        fullbright = lm is None
        # dynamic styles (animated 1..11, switchable 32..63): kept apart so the game can relight
        dyn = [(si, st) for si, st in enumerate(styles) if st != 0 and lm is not None][:2]
        lmraw = None
        if dyn:
            n = lw * lh * 3
            lmraw = np.stack([np.frombuffer(lighting, np.uint8, n, lofs + si * n).astype(np.float32).reshape(lh, lw, 3)
                              for si, _ in dyn])

        g = choose_grid(lm, bmin, grid, lm_tolerance) if (grid > 0 and not fullbright) else 0
        pieces = subdivide(poly, g) if g > 0 else [poly]
        face_grid_hist[g] = face_grid_hist.get(g, 0) + 1
        face_jobs[fi] = dict(pieces=[[tuple(float(x) for x in v[0]) for v in pc] for pc in pieces],
                             sv=sv, so=so, tv=tv, to=to, tex=tex, lm=lm, lw=lw, lh=lh, bmin=bmin,
                             nstyles=len(styles), fullbright=fullbright, planenum=planenum, side=side,
                             dyn=dyn, lmraw=lmraw, styles=styles,
                             flags=flags, miptex=miptex)
        out_faces.append(None)

    # ---- T-junction removal (like qbsp's tjunc.c): insert every vertex that lies on a polygon
    # edge into that edge, per brush model, so neighbouring polygons share edges exactly.
    CELL = 64.0
    for m in models:
        ff, nf = m[14], m[15]
        grid_pts = {}
        for fi in range(ff, ff + nf):
            job = face_jobs.get(fi)
            if not job:
                continue
            for pc in job['pieces']:
                for p in pc:
                    key = (int(math.floor(p[0] / CELL)), int(math.floor(p[1] / CELL)), int(math.floor(p[2] / CELL)))
                    grid_pts.setdefault(key, set()).add((round(p[0], 3), round(p[1], 3), round(p[2], 3)))
        for fi in range(ff, ff + nf):
            job = face_jobs.get(fi)
            if not job:
                continue
            newpieces = []
            for pc in job['pieces']:
                out, inserted = [], False
                n = len(pc)
                orig = []
                for i in range(n):
                    a, b = pc[i], pc[(i + 1) % n]
                    out.append(a)
                    orig.append(True)
                    d = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
                    l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2]
                    if l2 < 1e-6:
                        continue
                    lo = [int(math.floor((min(a[k], b[k]) - 0.1) / CELL)) for k in range(3)]
                    hi = [int(math.floor((max(a[k], b[k]) + 0.1) / CELL)) for k in range(3)]
                    found = []
                    for cx in range(lo[0], hi[0] + 1):
                        for cy in range(lo[1], hi[1] + 1):
                            for cz in range(lo[2], hi[2] + 1):
                                for q in grid_pts.get((cx, cy, cz), ()):
                                    w = (q[0] - a[0], q[1] - a[1], q[2] - a[2])
                                    t = (w[0] * d[0] + w[1] * d[1] + w[2] * d[2]) / l2
                                    if t <= 1e-4 or t >= 1 - 1e-4:
                                        continue
                                    px, py, pz = a[0] + d[0] * t - q[0], a[1] + d[1] * t - q[1], a[2] + d[2] * t - q[2]
                                    if px * px + py * py + pz * pz < 0.02 * 0.02:
                                        found.append((t, q))
                    if found:
                        found.sort()
                        last = -1
                        for t, q in found:
                            if t - last > 1e-4:
                                out.append(q)
                                orig.append(False)
                                last = t
                        inserted = True
                if inserted:
                    # rotate so the ring starts at a corner whose both edges are unsplit:
                    # a plain fan from there is valid (no extra vertex, no degenerate triangles)
                    m_ = len(out)
                    root = -1
                    for i in range(m_):
                        if orig[i] and orig[i - 1] and orig[(i + 1) % m_]:
                            root = i
                            break
                    if root >= 0:
                        out = out[root:] + out[:root]
                        inserted = False
                newpieces.append((out, inserted))
            job['pieces'] = newpieces

    # ---- build vertices / chunks
    for fi in range(len(faces)):
        job = face_jobs.get(fi)
        if not job:
            continue
        tex, lm, lw, lh, bmin = job['tex'], job['lm'], job['lw'], job['lh'], job['bmin']
        sv, so, tv, to = job['sv'], job['so'], job['tv'], job['to']
        fullbright = job['fullbright']
        tw, th = tex['tw'], tex['th']
        su, tvs = tw / tex['w'], th / tex['h']

        def vattr(p):
            s = p[0] * sv[0] + p[1] * sv[1] + p[2] * sv[2] + so
            t = p[0] * tv[0] + p[1] * tv[1] + p[2] * tv[2] + to
            dl = None
            if fullbright:
                col = (255, 255, 255)
            else:
                c = lm_sample(lm, lw, lh, job['nstyles'], s / 16 - bmin[0], t / 16 - bmin[1])
                col = tuple(GAMMA[min(1023, int(x))] for x in c)
                if job['dyn']:
                    # light from the dynamic styles at full strength, and everything else (base)
                    ls_, lt_ = s / 16 - bmin[0], t / 16 - bmin[1]
                    contrib = [lm_sample(job['lmraw'][k:k + 1], lw, lh, 1, ls_, lt_) for k in range(len(job['dyn']))]
                    on = sum(contrib[k] * style_on.get(job['dyn'][k][1], 1.0) for k in range(len(job['dyn'])))
                    base = c - on
                    dl = (base, [(job['dyn'][k][1], contrib[k]) for k in range(len(job['dyn']))])
            return p, s * su, t * tvs, col, dl

        # greedy chunking
        chunk_list = []  # list of (verts[list of attr], tris[list of idx triple])
        cur_keys, cur_verts, cur_tris = {}, [], []

        def flush():
            nonlocal cur_keys, cur_verts, cur_tris
            if cur_tris:
                chunk_list.append((cur_verts, cur_tris))
            cur_keys, cur_verts, cur_tris = {}, [], []

        for pc, inserted in job['pieces']:
            pts = list(pc)
            if inserted:
                # fan around the centroid so every (split) edge is kept exactly
                c = tuple(sum(p[k] for p in pts) / len(pts) for k in range(3))
                pts_all = pts + [c]
            else:
                pts_all = pts
            keys = [(round(p[0] * 64), round(p[1] * 64), round(p[2] * 64)) for p in pts_all]
            newcount = len({k for k in keys if k not in cur_keys})
            if len(cur_verts) + newcount > MAX_CHUNK_VERTS:
                flush()
            ids = []
            for k, p in zip(keys, pts_all):
                if k not in cur_keys:
                    cur_keys[k] = len(cur_verts)
                    cur_verts.append(vattr(p))
                ids.append(cur_keys[k])
            if inserted:
                ci = ids[-1]
                ring = ids[:-1]
                for j in range(len(ring)):
                    a, b = ring[j], ring[(j + 1) % len(ring)]
                    if a != b and a != ci and b != ci:
                        cur_tris.append((ci, a, b))
            else:
                for j in range(1, len(ids) - 1):
                    if ids[0] != ids[j] and ids[j] != ids[j + 1] and ids[0] != ids[j + 1]:
                        cur_tris.append((ids[0], ids[j], ids[j + 1]))
        flush()

        first_chunk = len(out_chunks)
        for cverts, ctris in chunk_list:
            us = [v[1] for v in cverts]; vs = [v[2] for v in cverts]
            uoff = math.floor(min(us) / tw) * tw
            voff = math.floor(min(vs) / th) * th
            if len(out_verts) & 1:
                out_verts.append(out_verts[-1])
            base = len(out_verts)
            for p, u, vv, col, dl in cverts:
                if dl is not None:
                    dyn_recs.append((len(out_verts), dl))
                ps = [max(-32768, min(32767, int(round(x * VERT_SCALE)))) for x in p]
                uu = int(round((u - uoff) * 32)); vv2 = int(round((vv - voff) * 32))
                uu = max(-32768, min(32767, uu)); vv2 = max(-32768, min(32767, vv2))
                out_verts.append((ps, col, (uu, vv2)))
            nv = len(cverts)
            if nv & 1:
                out_verts.append(out_verts[-1]); nv += 1
            out_chunks.append((base, nv, len(out_idx) // 3, len(ctris)))
            for tr in ctris:
                out_idx.extend(tr)
            face_tri_total += len(ctris)
        out_faces[fi] = (job['planenum'], job['side'], job['flags'], job['miptex'], first_chunk,
                         len(out_chunks) - first_chunk)

    # ---- write
    blobs = {}

    blobs['planes'] = b''.join(struct.pack(E + '4fB3x', p[0], p[1], p[2], p[3], p[4]) for p in planes)

    numnodes = len(nodes)
    cl = []
    for n in nodes:  # hull 0 from nodes
        ch = []
        for c in (n[1], n[2]):
            ch.append(c if c >= 0 else leafs[-c - 1][0])
        cl.append(struct.pack(E + '3i', n[0], ch[0], ch[1]))
    for c in clipnodes:
        ch = [x + numnodes if x >= 0 else x for x in (c[1], c[2])]
        cl.append(struct.pack(E + '3i', c[0], ch[0], ch[1]))
    blobs['clipnodes'] = b''.join(cl)

    blobs['nodes'] = b''.join(struct.pack(E + 'i2h3h3hHH', *n) for n in nodes)
    blobs['leafs'] = b''.join(struct.pack(E + 'ii3h3hHH4B', *lf) for lf in leafs)
    blobs['marks'] = b''.join(struct.pack(E + 'H', m[0]) for m in marks)
    blobs['vis'] = vis
    mb = []
    for m in models:
        mins, maxs, org = m[0:3], m[3:6], m[6:9]
        head = list(m[9:13])
        head = [head[0]] + [h + numnodes if h >= 0 else h for h in head[1:]]
        mb.append(struct.pack(E + '9f4i3i', *mins, *maxs, *org, *head, m[13], m[14], m[15]))
    blobs['models'] = b''.join(mb)
    blobs['faces'] = b''.join(struct.pack(E + 'HBBHHIHxx', f[0], f[1], f[2], f[3] & 0xffff, 0, f[4], f[5]) for f in out_faces)
    blobs['facetex'] = b''.join(struct.pack(E + '8f5i', *a, *b, c, d, e, g, h) for (a, b, c, d, e, g, h) in out_facetex)
    blobs['chunks'] = b''.join(struct.pack(E + 'IBBHI', c[0], c[1], c[3], 0, c[2]) for c in out_chunks)
    vb = bytearray()
    for i in range(0, len(out_verts), 2):
        a = out_verts[i]; b = out_verts[i + 1] if i + 1 < len(out_verts) else out_verts[i]
        vb += struct.pack(E + '3hH3hH', *a[0], 0, *b[0], 0)
        vb += struct.pack(E + '4B4B', *a[1], 255, *b[1], 255)
        vb += struct.pack(E + '2h2h', *a[2], *b[2])
    blobs['verts'] = bytes(vb)
    blobs['idx'] = bytes(out_idx)
    blobs['entities'] = ent_text.encode('latin1') + b'\0'
    # light styles: the gamma table, then per relit vertex its base light and up to two styles
    lsb = bytearray(bytes(GAMMA[i] for i in range(1024)))
    # packed: u32 vi:19 | s0:6 | s1:6 | two:1, then u32 base rgb (10 bits each), u32 style-0 rgb,
    # and u32 style-1 rgb only when there are two styles
    def rgb10(c):
        q = [max(0, min(1023, int(round(x)))) for x in c]
        return q[0] << 20 | q[1] << 10 | q[2]
    for vi, (base, sl) in dyn_recs:
        two = len(sl) > 1
        w0 = vi | (sl[0][0] & 63) << 19 | ((sl[1][0] & 63) << 25 if two else 0) | (1 << 31 if two else 0)
        lsb += struct.pack(E + 'III', w0, rgb10(base), rgb10(sl[0][1]))
        if two:
            lsb += struct.pack(E + 'I', rgb10(sl[1][1]))
    blobs['lightstyles'] = bytes(lsb)
    counts = dict(planes=len(planes), clipnodes=len(cl), nodes=numnodes, leafs=len(leafs), marks=len(marks),
                  vis=len(vis), models=len(models), faces=len(out_faces), facetex=len(out_facetex),
                  chunks=len(out_chunks), verts=(len(out_verts) + 1) // 2 * 2, idx=len(out_idx) // 3,
                  entities=len(blobs['entities']), lightstyles=len(dyn_recs))
    stats = dict(faces=len(out_faces), tris=face_tri_total, verts=len(out_verts), grids=face_grid_hist)
    return dict(name=os.path.splitext(os.path.basename(bsp_path))[0], blobs=blobs, counts=counts,
                textures=textures, stats=stats)


class TexturePack:
    """Shared, de-duplicated store of converted textures (CI8 texels + RGBA5551 palettes)."""
    def __init__(self):
        self.data = bytearray()
        self.index = {}

    def _put(self, b):
        while len(self.data) % 16:
            self.data.append(0)
        o = len(self.data)
        self.data += b
        return o

    def add(self, t):
        key = (t['data'], t['pal'])
        if key not in self.index:
            self.index[key] = (self._put(t['data']), self._put(t['pal']))
        return self.index[key]


def write_hlb(res, out_path, texpack=None, verbose=True):
    """Assemble the final .hlb. With a texpack, texture texels live in the shared pack (version 2)."""
    blobs, counts = dict(res['blobs']), dict(res['counts'])
    th, td = bytearray(), bytearray()
    for t in res['textures']:
        if t is None:
            t = dict(name='', w=16, h=16, tw=8, th=4, data=bytes(32), pal=bytes(512), flags=FF_NODRAW)
        if texpack is not None:
            dofs, pofs = texpack.add(t)
        else:
            while len(td) % 16:
                td.append(0)
            dofs = len(td); td += t['data']
            while len(td) % 16:
                td.append(0)
            pofs = len(td); td += t['pal']
        th += struct.pack(E + '16sHHHHHHII', t['name'].encode('latin1')[:15], t['tw'], t['th'], t['w'], t['h'],
                          t['flags'], 0, dofs, pofs)
    blobs['textures'] = bytes(th)
    blobs['texdata'] = bytes(td)
    counts['textures'] = len(res['textures'])
    counts['texdata'] = len(td)

    order = ['planes', 'clipnodes', 'nodes', 'leafs', 'marks', 'vis', 'models', 'faces', 'facetex',
             'chunks', 'verts', 'idx', 'textures', 'texdata', 'entities', 'lightstyles']
    hdr_size = 16 + 8 * len(order)
    version = 2 if texpack is not None else 1
    out = bytearray(b'HL64' + struct.pack(E + 'III', version, VERT_SCALE, len(order)))
    body = bytearray()
    pos = hdr_size
    while pos % 16:
        pos += 1
    table = []
    for k in order:
        while (pos + len(body)) % 16:
            body.append(0)
        table.append((pos + len(body), counts[k]))
        body += blobs[k]
    for o, c in table:
        out += struct.pack(E + 'II', o, c)
    while len(out) < pos:
        out.append(0)
    out += body
    open(out_path, 'wb').write(out)
    if verbose:
        st = res['stats']
        print('%s: %d faces, %d tris, %d verts, %d textures, %d KB  grids %s' % (
            res['name'], st['faces'], st['tris'], st['verts'], len(res['textures']), len(out) // 1024,
            dict(sorted(st.get('grids', {}).items()))))
    return out


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('bsp'); ap.add_argument('out')
    ap.add_argument('--wad', action='append', default=[])
    ap.add_argument('--grid', type=int, default=64, help='finest lighting subdivision grid in texels (0=off)')
    ap.add_argument('--tol', type=float, default=12, help='allowed lighting error (0-255) before subdividing')
    ap.add_argument('--maxtexels', type=int, default=2048)
    ap.add_argument('--little-endian', action='store_true', help='for the PC test harness only')
    a = ap.parse_args()
    if a.little_endian:
        E = '<'
    write_hlb(convert(a.bsp, a.out, a.wad, a.grid, a.maxtexels, lm_tolerance=a.tol), a.out)
