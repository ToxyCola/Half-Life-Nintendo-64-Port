#!/usr/bin/env python3
"""
mdl2n64.py - convert a Half-Life studio model (.mdl, v10) for the HL64 N64 engine.

* Skeleton: bone parents. Vertices stay in bone-local space exactly as in the .mdl, so the
  N64 transforms each bone's vertices with that bone's matrix (classic N64 skinning).
* Meshes: split into chunks of <= 60 vertices (grouped by bone) for tiny3d's vertex cache.
* Textures: 8-bit paletted, resampled to fit TMEM as CI8 (same as the world).
* Animations: only the sequences the game needs (idle/walk/run, sequences named by the maps'
  scripted_sequences, dead poses ...), decoded to one quaternion + position per bone per frame.
  Decoding follows Valve's StudioCalcBoneQuaterion / StudioCalcBonePosition.

Output: big-endian 'HLM1' file.
"""
import struct, sys, os, math
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bsp2n64 import convert_texture

E = '>'
POS_SCALE = 8          # int16 vertex/bone positions are units * 8
MAX_CHUNK_VERTS = 60

ACT_IDLE, ACT_WALK, ACT_RUN = 1, 3, 4
STUDIO_X, STUDIO_Y, STUDIO_Z = 1, 2, 4
NF_MASKED = 0x40
NF_ADDITIVE = 0x20
NF_CHROME = 0x02


class Mdl:
    def __init__(self, path):
        self.path = path
        self.d = open(path, 'rb').read()
        d = self.d
        (self.id, self.version) = struct.unpack_from('<4si', d, 0)
        if self.id != b'IDST' or self.version != 10:
            raise ValueError('%s: not a v10 studio model' % path)
        f = struct.unpack_from('<64si15fi', d, 8)
        self.name = f[0].split(b'\0')[0].decode('latin1')
        self.eye = f[2:5]
        self.hullmin, self.hullmax = f[5:8], f[8:11]
        off = 8 + 64 + 4 + 12 * 5 + 4
        (self.numbones, self.boneindex, self.numbonecontrollers, self.bonecontrollerindex,
         self.numhitboxes, self.hitboxindex, self.numseq, self.seqindex, self.numseqgroups,
         self.seqgroupindex, self.numtextures, self.textureindex, self.texturedataindex,
         self.numskinref, self.numskinfamilies, self.skinindex, self.numbodyparts,
         self.bodypartindex, self.numattachments, self.attachmentindex, _, _, _, _,
         self.numtransitions, self.transitionindex) = struct.unpack_from('<26i', d, off)

    def bones(self):
        out = []
        for i in range(self.numbones):
            o = self.boneindex + i * 112
            name = self.d[o:o + 32].split(b'\0')[0].decode('latin1')
            parent, flags = struct.unpack_from('<ii', self.d, o + 32)
            ctrl = struct.unpack_from('<6i', self.d, o + 40)
            value = struct.unpack_from('<6f', self.d, o + 64)
            scale = struct.unpack_from('<6f', self.d, o + 88)
            out.append(dict(name=name, parent=parent, value=value, scale=scale))
        return out

    def seqs(self):
        out = []
        for i in range(self.numseq):
            o = self.seqindex + i * 176
            label = self.d[o:o + 32].split(b'\0')[0].decode('latin1')
            fps, flags, activity, actweight, numevents, eventindex, numframes, numpivots, pivotindex, \
                motiontype, motionbone = struct.unpack_from('<f10i', self.d, o + 32)
            linmove = struct.unpack_from('<3f', self.d, o + 76)
            numblends, animindex = struct.unpack_from('<2i', self.d, o + 120)
            blendtype = struct.unpack_from('<2i', self.d, o + 128)
            blendstart = struct.unpack_from('<2f', self.d, o + 136)
            blendend = struct.unpack_from('<2f', self.d, o + 144)
            events = []
            for ev in range(numevents):
                eo = eventindex + ev * 76
                efr, eev, etype = struct.unpack_from('<3i', self.d, eo)
                opt = self.d[eo + 12:eo + 76].split(b'\0')[0].decode('latin1')
                events.append((efr, eev, opt))
            seqgroup = struct.unpack_from('<i', self.d, o + 156)[0]
            out.append(dict(label=label, fps=fps, flags=flags, activity=activity, numframes=numframes,
                            motiontype=motiontype, motionbone=motionbone, linmove=linmove,
                            numblends=numblends, animindex=animindex, seqgroup=seqgroup, events=events,
                            blendtype=blendtype, blendstart=blendstart, blendend=blendend))
        return out


def anim_value(buf, base, k):
    """Valve's run-length 'mstudioanimvalue_t' decode for frame k (shorts, little endian)."""
    i = base
    while True:
        valid, total = buf[i], buf[i + 1]
        if total < valid:
            k = 0
        if total <= k:
            k -= total
            i += (valid + 1) * 2
            continue
        break
    if valid > k:
        return struct.unpack_from('<h', buf, i + (k + 1) * 2)[0]
    return struct.unpack_from('<h', buf, i + valid * 2)[0]


def angle_quat(a):
    # AngleQuaternion() from Valve's cl_dll/studio_util.cpp
    sy, cy = math.sin(a[2] * 0.5), math.cos(a[2] * 0.5)
    sp, cp = math.sin(a[1] * 0.5), math.cos(a[1] * 0.5)
    sr, cr = math.sin(a[0] * 0.5), math.cos(a[0] * 0.5)
    return (sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy, cr * cp * sy - sr * sp * cy,
            cr * cp * cy + sr * sp * sy)


STUDIO_XR = 0x8


def quat_slerp(p, q, t):
    """QuaternionSlerp from Valve's studio_util.cpp"""
    a = sum((p[i] - q[i]) ** 2 for i in range(4))
    b = sum((p[i] + q[i]) ** 2 for i in range(4))
    if a > b:
        q = tuple(-x for x in q)
    cosom = sum(p[i] * q[i] for i in range(4))
    if 1.0 + cosom > 1e-6:
        if 1.0 - cosom > 1e-6:
            omega = math.acos(max(-1.0, min(1.0, cosom)))
            sinom = math.sin(omega)
            sclp, sclq = math.sin((1.0 - t) * omega) / sinom, math.sin(t * omega) / sinom
        else:
            sclp, sclq = 1.0 - t, t
        return tuple(sclp * p[i] + sclq * q[i] for i in range(4))
    qt = (-q[1], q[0], -q[3], q[2])
    sclp, sclq = math.sin((1.0 - t) * 0.5 * math.pi), math.sin(t * 0.5 * math.pi)
    return tuple(sclp * p[i] + sclq * qt[i] for i in range(3)) + (qt[3],)


def decode_frames(mdl, groupdata, seq, bones):
    """Return [frame][bone] = (quat(4), pos(3)). Sequences that blend between two aiming pitches
    (soldiers' and Barney's shooting) are mixed at pitch 0, aiming level, as SetBlending(0, 0)
    would on the PC; others use their only blend."""
    if seq.get('numblends', 1) >= 2 and seq['blendtype'][0] & STUDIO_XR and seq['blendend'][0] != seq['blendstart'][0]:
        t = (0.0 - seq['blendstart'][0]) / (seq['blendend'][0] - seq['blendstart'][0])
        t = max(0.0, min(1.0, t))
        a = _decode_blend(groupdata, seq, bones, 0)
        b = _decode_blend(groupdata, seq, bones, 1)
        return [[(quat_slerp(qa, qb, t), [pa[k] + (pb[k] - pa[k]) * t for k in range(3)])
                 for (qa, pa), (qb, pb) in zip(fa, fb)] for fa, fb in zip(a, b)]
    return _decode_blend(groupdata, seq, bones, 0)


def _decode_blend(groupdata, seq, bones, blend):
    buf = groupdata
    animbase = seq['animindex'] + blend * len(bones) * 12
    frames = []
    for fr in range(seq['numframes']):
        fb = []
        for bi, b in enumerate(bones):
            ao = animbase + bi * 12
            offs = struct.unpack_from('<6H', buf, ao)
            vals = []
            for j in range(6):
                v = b['value'][j]
                if offs[j]:
                    v = v + anim_value(buf, ao + offs[j], fr) * b['scale'][j]
                vals.append(v)
            pos = vals[0:3]
            if bi == seq['motionbone']:
                if seq['motiontype'] & STUDIO_X: pos[0] = 0.0
                if seq['motiontype'] & STUDIO_Y: pos[1] = 0.0
                if seq['motiontype'] & STUDIO_Z: pos[2] = 0.0
            fb.append((angle_quat(vals[3:6]), pos))
        frames.append(fb)
    return frames


def i16(x):
    return max(-32768, min(32767, int(round(x))))


def convert(mdl_path, out_path, wanted_names=(), max_texels=2048, verbose=True):
    m = Mdl(mdl_path)
    base = os.path.splitext(mdl_path)[0]
    tex_m = m
    if m.numtextures == 0 and os.path.exists(base + 't.mdl'):
        tex_m = Mdl(base + 't.mdl')
    elif m.numtextures == 0 and os.path.exists(base + 'T.mdl'):
        tex_m = Mdl(base + 'T.mdl')
    bones = m.bones()
    seqs = m.seqs()

    # ---- which sequences to keep
    wanted = {n.lower() for n in wanted_names if not n.startswith('#') and not n.startswith('@')}
    wanted_idx = {int(n[1:]) for n in wanted_names if n.startswith('#') and n[1:].isdigit()}
    # '@N' keeps the first two sequences of activity N (the AI's attacks, flinches, deaths), '*' all
    acts = {ACT_IDLE, ACT_WALK, ACT_RUN} | {int(n[1:]) for n in wanted_names if n.startswith('@') and n[1:].isdigit()}
    keep_all = '*' in wanted_names
    keep = []
    first_act = {}
    for i, s in enumerate(seqs):
        lab = s['label'].lower()
        base_act = s['activity'] in (ACT_IDLE, ACT_WALK, ACT_RUN)
        k = (keep_all or i == 0 or lab in wanted or i in wanted_idx or
             (s['activity'] in acts and first_act.get(s['activity'], 0) < (2 if base_act else 1)))
        if s['activity'] in acts:
            first_act[s['activity']] = first_act.get(s['activity'], 0) + 1
        if k:
            keep.append(i)
    groups = {0: m.d}

    def group_data(g):
        if g not in groups:
            p = '%s%02d.mdl' % (base, g)
            groups[g] = open(p, 'rb').read() if os.path.exists(p) else None
        return groups[g]

    seq_out = []
    animblob = bytearray()
    eventblob = bytearray()
    for i in keep:
        s = seqs[i]
        gd = group_data(s['seqgroup'])
        if gd is None or s['numframes'] <= 0:
            continue
        frames = decode_frames(m, gd, s, bones)
        fps = s['fps'] if s['fps'] > 0 else 10
        step = 1
        # RAM is tight (8 MB for map + characters): long sequences keep every 2nd or 3rd frame,
        # the run-time interpolation between frames hides it. First-person weapons keep all.
        if not keep_all:
            if len(frames) > 90:
                step = 3
            elif len(frames) > 24:
                step = 2
        elif len(frames) > 40:              # view models: the long idles / reloads / fidgets
            step = 2
        frames = frames[::step]
        # compact keys: rotation as x,y,z of a w>=0 quaternion (w rebuilt at run time); positions
        # only for bones whose position actually changes during the sequence (usually just the root)
        nb = len(frames[0])
        posmask = [0] * nb
        for b in range(nb):
            p0 = frames[0][b][1]
            for fb in frames[1:]:
                if max(abs(fb[b][1][k] - p0[k]) for k in range(3)) * POS_SCALE >= 0.5:
                    posmask[b] = 1
                    break
        while len(animblob) % 2: animblob.append(0)
        ofs = len(animblob)
        animblob += bytes(posmask) + (b'\0' if nb % 2 else b'')
        for b in range(nb):
            animblob += struct.pack(E + '3h', *[i16(c * POS_SCALE) for c in frames[0][b][1]])
        for fb in frames:
            for q, p in fb:
                if q[3] < 0: q = [-c for c in q]
                animblob += struct.pack(E + '3h', *[i16(c * 32767) for c in q[:3]])
            for b in range(nb):
                if posmask[b]:
                    animblob += struct.pack(E + '3h', *[i16(c * POS_SCALE) for c in fb[b][1]])
        lm = s['linmove']
        # animation events that matter without NPC AI: 1003 = fire a target (SCRIPT_EVENT_FIREEVENT),
        # 1004 = play a sound, 1008 = sentence (kept for completeness)
        evs = [(fr // step, ev, opt) for fr, ev, opt in s['events'] if ev in (1003, 1004, 1005, 1008, 1009)]
        eofs = len(eventblob)
        for fr, ev, opt in evs:
            eventblob += struct.pack(E + 'HH60s', fr, ev, opt.encode('latin1')[:59])
        seq_out.append(struct.pack(E + '32sfIHH3fIHHI', s['label'].encode('latin1')[:31], fps / step, s['flags'],
                                   len(frames), s['activity'], lm[0], lm[1], lm[2], ofs, len(evs), i, eofs))

    # ---- textures
    textures, tex_out = [], bytearray()
    tex_hdr = []
    for t in range(tex_m.numtextures):
        o = tex_m.textureindex + t * 80
        name = tex_m.d[o:o + 64].split(b'\0')[0].decode('latin1')
        flags, w, h, index = struct.unpack_from('<4i', tex_m.d, o + 64)
        pix = np.frombuffer(tex_m.d, np.uint8, w * h, index).reshape(h, w)
        pal = np.frombuffer(tex_m.d, np.uint8, 768, index + w * h).reshape(256, 3)
        if flags & NF_CHROME:
            # no environment mapping on N64: a flat texture in the chrome's average colour
            mean = pal[pix].reshape(-1, 3).mean(0)
            idx = int(((pal.astype(np.float32) - mean) ** 2).sum(1).argmin())
            pix = np.full((h, w), idx, np.uint8)
            flags &= ~NF_CHROME
        tw, th, data, p16 = convert_texture(('{' if flags & NF_MASKED else '') + name, w, h, pix, pal, max_texels)
        while len(tex_out) % 16: tex_out.append(0)
        dofs = len(tex_out); tex_out += data
        while len(tex_out) % 16: tex_out.append(0)
        pofs = len(tex_out); tex_out += p16
        tex_hdr.append(struct.pack(E + 'HHHHII', tw, th, w, h, dofs, pofs) +
                       struct.pack(E + 'I', flags))
        textures.append((w, h, tw, th))
    skins = struct.unpack_from('<%dh' % (tex_m.numskinref * tex_m.numskinfamilies), tex_m.d, tex_m.skinindex) \
        if tex_m.numskinref else ()

    # ---- meshes -> chunks
    verts = []       # (pos(3) int16, normal packed, s, t)
    loads = []       # (bone, cacheofs, count, vertofs)
    tris = []        # cache indices
    chunks = []      # (firstload, numloads, firsttri, numtris, skinref, numverts)
    bodyparts = []   # (nummodels, base, firstmodel)
    models_out = []  # (firstchunk, numchunks)
    total_tris = 0

    def pack_normal(n):
        x = max(-16, min(15, int(round(n[0] * 15.5))))
        y = max(-32, min(31, int(round(n[1] * 31.5))))
        z = max(-16, min(15, int(round(n[2] * 15.5))))
        return ((x & 31) << 11) | ((y & 63) << 5) | (z & 31)

    for bp in range(m.numbodyparts):
        o = m.bodypartindex + bp * 76
        nummodels, bbase, modelindex = struct.unpack_from('<3i', m.d, o + 64)
        bodyparts.append((nummodels, bbase, len(models_out)))
        for mi in range(nummodels):
            mo = modelindex + mi * 112
            (mtype, radius, nummesh, meshindex, numverts, vertinfoindex, vertindex, numnorms, norminfoindex,
             normindex, numgroups, groupindex) = struct.unpack_from('<if10i', m.d, mo + 64)
            vbone = m.d[vertinfoindex:vertinfoindex + numverts]
            vpos = [struct.unpack_from('<3f', m.d, vertindex + 12 * i) for i in range(numverts)]
            vnorm = [struct.unpack_from('<3f', m.d, normindex + 12 * i) for i in range(numnorms)]
            first_chunk = len(chunks)
            for me in range(nummesh):
                numtris_, triindex, skinref, mnumnorms, mnormindex = struct.unpack_from('<5i', m.d, meshindex + me * 20)
                tw0, th0, tw, th = textures[skins[skinref]] if skins and skins[skinref] < len(textures) else (64, 64, 64, 64)
                # decode tri commands (strips / fans) into triangles of (vert, norm, s, t)
                ptr = triindex
                mtris = []
                while True:
                    cnt = struct.unpack_from('<h', m.d, ptr)[0]; ptr += 2
                    if cnt == 0:
                        break
                    fan = cnt < 0
                    cnt = abs(cnt)
                    pts = []
                    for _ in range(cnt):
                        pts.append(struct.unpack_from('<4h', m.d, ptr)); ptr += 8
                    for j in range(2, cnt):
                        if fan:
                            t = (pts[0], pts[j - 1], pts[j])
                        elif j % 2:
                            t = (pts[j - 1], pts[j - 2], pts[j])
                        else:
                            t = (pts[j - 2], pts[j - 1], pts[j])
                        mtris.append(t)
                # greedy chunking: <= MAX_CHUNK_VERTS unique verts (incl. per-bone even padding)
                pending = mtris
                while pending:
                    keyset = {}
                    chosen = []
                    rest = []
                    def cost(ks):
                        per = {}
                        for k in ks:
                            per[vbone[k[0]]] = per.get(vbone[k[0]], 0) + 1
                        return sum(c + (c & 1) for c in per.values())
                    for t in pending:
                        if rest:
                            rest.append(t); continue
                        new = dict(keyset)
                        for p in t:
                            new[p] = 1
                        if cost(new) > MAX_CHUNK_VERTS and chosen:
                            rest.append(t); continue
                        keyset = new
                        chosen.append(t)
                    pending = rest
                    # lay out vertices grouped by bone
                    bybone = {}
                    for k in keyset:
                        bybone.setdefault(vbone[k[0]], []).append(k)
                    cache = {}
                    first_load = len(loads)
                    cofs = 0
                    for bone in sorted(bybone):
                        ks = bybone[bone]
                        if len(verts) & 1:
                            verts.append(verts[-1])
                        vofs = len(verts)
                        for k in ks:
                            vi, ni, s_, t_ = k
                            p = vpos[vi]
                            n = vnorm[ni] if ni < len(vnorm) else (0, 0, 1)
                            verts.append(([i16(c * POS_SCALE) for c in p], pack_normal(n),
                                          i16(s_ * tw / tw0 * 32), i16(t_ * th / th0 * 32)))
                            cache[k] = cofs
                            cofs += 1
                        if len(ks) & 1:
                            verts.append(verts[-1]); cofs += 1
                        loads.append((bone, cofs - len(ks) - (len(ks) & 1), len(ks) + (len(ks) & 1), vofs))
                    first_tri = len(tris) // 3
                    for t in chosen:
                        tris.extend(cache[p] for p in t)
                    chunks.append((first_load, len(loads) - first_load, first_tri, len(chosen),
                                   skinref, cofs))
                    total_tris += len(chosen)
            models_out.append((first_chunk, len(chunks) - first_chunk))

    # ---- write
    blob = {}
    blob['bones'] = b''.join(struct.pack(E + 'h', b['parent']) for b in bones)
    blob['seqs'] = b''.join(seq_out)
    blob['anim'] = bytes(animblob)
    blob['tex'] = b''.join(tex_hdr)
    blob['texdata'] = bytes(tex_out)
    blob['skins'] = b''.join(struct.pack(E + 'h', s) for s in skins)
    blob['bodyparts'] = b''.join(struct.pack(E + 'HHI', *b) for b in bodyparts)
    blob['models'] = b''.join(struct.pack(E + 'II', *mo) for mo in models_out)
    blob['chunks'] = b''.join(struct.pack(E + 'IHHIHH', c[0], c[1], c[5], c[2], c[3], c[4]) for c in chunks)
    blob['loads'] = b''.join(struct.pack(E + 'BBBxI', *l) for l in loads)
    vb = bytearray()
    if len(verts) & 1:
        verts.append(verts[-1])
    for i in range(0, len(verts), 2):
        a, b = verts[i], verts[i + 1]
        vb += struct.pack(E + '3hH3hH', *a[0], a[1], *b[0], b[1])
        vb += struct.pack(E + 'II', 0xffffffff, 0xffffffff)
        vb += struct.pack(E + '2h2h', a[2], a[3], b[2], b[3])
    blob['verts'] = bytes(vb)
    blob['tris'] = bytes(tris)
    blob['events'] = bytes(eventblob)
    # bone controller 4 = the mouth (CBaseMonster::MouthOpen / SetController(4))
    ctl = b''
    for ci in range(m.numbonecontrollers):
        cb, ct, cs, ce, crest, cidx = struct.unpack_from('<2i2f2i', m.d, m.bonecontrollerindex + ci * 24)
        if cidx == 4 and (ct & 0x38):
            ctl = struct.pack(E + 'hHff', cb, ct, cs, ce)
    blob['ctrl'] = ctl
    # attachments (muzzles: muzzle flashes, the gluon beam start): bone + offset in the bone's frame
    att = b''
    for ai in range(m.numattachments):
        o = m.attachmentindex + ai * 88
        _, abone = struct.unpack_from('<ii', m.d, o + 32)
        ax, ay, az = struct.unpack_from('<3f', m.d, o + 40)
        att += struct.pack(E + 'hh3f', abone, 0, ax, ay, az)
    blob['att'] = att
    # bone controller 0 when it moves a bone (barnacle tongue: CBarnacle SetBoneController(0, ...))
    ctl0 = b''
    for ci in range(m.numbonecontrollers):
        cb, ct, cs, ce, crest, cidx = struct.unpack_from('<2i2f2i', m.d, m.bonecontrollerindex + ci * 24)
        if cidx == 0 and (ct & 0x07):
            ctl0 = struct.pack(E + 'hHff', cb, ct, cs, ce)
    blob['ctl0'] = ctl0
    order = ['bones', 'seqs', 'anim', 'tex', 'texdata', 'skins', 'bodyparts', 'models', 'chunks', 'loads',
             'verts', 'tris', 'events', 'ctrl', 'att', 'ctl0']
    counts = dict(bones=len(bones), seqs=len(seq_out), anim=len(animblob), tex=len(tex_hdr), texdata=len(tex_out),
                  skins=len(skins), bodyparts=len(bodyparts), models=len(models_out), chunks=len(chunks),
                  loads=len(loads), verts=len(verts), tris=len(tris) // 3, events=len(eventblob) // 64, ctrl=1 if ctl else 0, att=len(att) // 16, ctl0=1 if ctl0 else 0)
    hdr = bytearray(b'HLM1' + struct.pack(E + 'IIII', POS_SCALE, tex_m.numskinref, tex_m.numskinfamilies, len(order)))
    hdr += struct.pack(E + '9f', *m.eye, *m.hullmin, *m.hullmax)
    hdrsize = len(hdr) + 8 * len(order)
    pos = (hdrsize + 15) & ~15
    body = bytearray()
    table = []
    for k in order:
        while (pos + len(body)) % 16:
            body.append(0)
        table.append((pos + len(body), counts[k]))
        body += blob[k]
    for o_, c in table:
        hdr += struct.pack(E + 'II', o_, c)
    while len(hdr) < pos:
        hdr.append(0)
    out = bytes(hdr + body)
    open(out_path, 'wb').write(out)
    if verbose:
        print('%s: %d bones, %d/%d seqs, %d tris, %d verts, %d textures, %d KB' % (
            os.path.basename(mdl_path), len(bones), len(seq_out), len(seqs), total_tris, len(verts),
            len(tex_hdr), len(out) // 1024))
    return out


if __name__ == '__main__':
    convert(sys.argv[1], sys.argv[2], sys.argv[3:])
