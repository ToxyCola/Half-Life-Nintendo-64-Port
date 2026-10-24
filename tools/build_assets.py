#!/usr/bin/env python3
"""
build_assets.py - convert a Half-Life install into the HL64 ROM filesystem.

  python3 tools/build_assets.py <path to Half-Life/valve> filesystem/ [--maps c1a0 c1a1 ...]

Produces:
  filesystem/maps/<map>.hlb     per-map geometry, collision, vis, entities (big-endian)
  filesystem/maps/maplist.txt   maps in campaign order (first = "New game")
  filesystem/textures.pak       shared de-duplicated CI8 textures + palettes
  filesystem/sound/...          wav64 sounds (via libdragon's audioconv64) + materials.txt
"""
import os, sys, glob, argparse, subprocess, shutil, tempfile
from multiprocessing import Pool
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bsp2n64
import sky2n64
import mdl2n64
import ui2n64
import spr2n64
import re

CAMPAIGN = """c0a0 c0a0a c0a0b c0a0c c0a0d c0a0e c1a0 c1a0d c1a0a c1a0b c1a0e c1a0c c1a1 c1a1a c1a1f c1a1b c1a1c
c1a1d c1a2 c1a2a c1a2b c1a2c c1a2d c1a3 c1a3a c1a3b c1a3c c1a3d c1a4 c1a4k c1a4b c1a4f c1a4d c1a4e c1a4i
c1a4g c1a4j c2a1 c2a1a c2a1b c2a2 c2a2a c2a2b1 c2a2b2 c2a2c c2a2d c2a2e c2a2f c2a2g c2a2h c2a3 c2a3a c2a3b
c2a3c c2a3d c2a3e c2a4 c2a4a c2a4b c2a4c c2a4d c2a4e c2a4f c2a4g c2a5 c2a5w c2a5x c2a5a c2a5b c2a5c c2a5d
c2a5e c2a5f c2a5g c3a1 c3a1a c3a1b c3a2e c3a2 c3a2a c3a2b c3a2c c3a2d c3a2f c4a1 c4a2 c4a2a c4a2b c4a1a
c4a1b c4a1c c4a1d c4a1e c4a1f c4a3 c5a1 t0a0 t0a0a t0a0b t0a0b1 t0a0b2 t0a0c t0a0d""".split()



def work(args):
    bsp, wads, grid, tol = args
    return bsp2n64.convert(bsp, None, wads, grid, 2048, verbose=False, lm_tolerance=tol)


GENERIC_MODEL_CLASSES = ('monster_generic', 'cycler', 'monster_furniture', 'env_model', 'cycler_sprite')
EXTRA_SEQS = ['retract', 'delpoy', 'hide', 'spin', 'lying_on_back', 'lying_on_stomach', 'lying_on_side', 'dead_sitting', 'dead_hang', 'dead_table1',
              'dead_table2', 'dead_table3', 'deadstomach', 'deadside', 'deadsitting', 'deadback', 'deadtable',
              'sitting2', 'sitlookleft']


def slashes(p):
    """libdragon's tools split paths on '/' only: give them forward slashes on Windows too"""
    return p.replace('\\', '/') if os.sep == '\\' else p


def npc_model_table():
    """classname -> model, read from src/npc.c so the table lives in one place"""
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'npc.c'), encoding='latin1').read()
    return dict(re.findall(r'\{"(\w+)", "([\w()]+)", \{', src))


def model_conv(args):
    path, out, wanted = args
    try:
        mdl2n64.convert(path, out, wanted, verbose=False)
        return out, None
    except Exception as e:  # noqa
        return out, repr(e)


def build_models(a, maps):
    table = npc_model_table()
    used = {}            # model -> set of wanted sequence names / '#index'
    names = set(EXTRA_SEQS)
    # activities the monster AI plays (src/npc.c): flinches, attacks, deaths, combat idles, fear
    names |= {'@%d' % a for a in (8, 25, 26, 28, 29, 30, 31, 36, 37, 62, 64, 5)}
    for m in maps:
        b = bsp2n64.BSP(open(os.path.join(a.valve, 'maps', m + '.bsp'), 'rb').read())
        ents = bsp2n64.parse_entities(b.raw('ENTITIES').split(b'\0')[0].decode('latin1'))
        for e in ents:
            cls = e.get('classname', '').lower()
            mdl = None
            if cls in table:
                mdl = table[cls]
            elif cls in GENERIC_MODEL_CLASSES and e.get('model', '').lower().startswith('models/'):
                mdl = os.path.splitext(os.path.basename(e['model'].lower()))[0]
            if mdl:
                ws = used.setdefault(mdl, set())
                if e.get('sequence', '').strip().isdigit():
                    ws.add('#' + e['sequence'].strip())
            if cls in ('scripted_sequence', 'aiscripted_sequence'):
                for k in ('m_iszPlay', 'm_iszIdle'):
                    if e.get(k):
                        names.add(e[k].lower())
        for e in ents:
            if e.get('classname', '').lower() == 'world_items':
                t = e.get('type', '').strip()
                mdl = {'42': 'w_antidote', '43': 'w_security', '44': 'w_battery', '45': 'w_suit'}.get(t)
                if mdl:
                    used.setdefault(mdl, set())
    # everything the player can pick up, throw or see in first person (crates and soldiers drop
    # items, so all of them go in, not only the ones the maps place)
    for cls, mdl in table.items():
        if cls.startswith(('weapon_', 'ammo_', 'item_')) or cls == 'weaponbox':
            used.setdefault(mdl, set())
    for mdl in ('v_crowbar', 'v_9mmhandgun', 'v_357', 'v_9mmar', 'v_shotgun', 'v_crossbow', 'v_rpg', 'v_gauss', 'v_egon',
                'v_hgun', 'v_grenade', 'v_tripmine', 'v_satchel', 'v_satchel_radio', 'v_squeak'):
        used.setdefault(mdl, set()).add('*')
    for mdl in ('grenade', 'rpgrocket', 'crossbow_bolt', 'hornet', 'w_squeak', 'w_satchel', 'w_grenade', 'hgibs', 'agibs'):
        used.setdefault(mdl, set())
    os.makedirs(os.path.join(a.out, 'models'), exist_ok=True)
    jobs = []
    for mdl, ws in sorted(used.items()):
        path = os.path.join(a.valve, 'models', mdl + '.mdl')
        if not os.path.exists(path):
            print('model %s not found' % mdl)
            continue
        jobs.append((path, os.path.join(a.out, 'models', mdl + '.hlm'), sorted(names | ws)))
    outs = []
    with Pool(a.j) as pool:
        for out, err in pool.imap_unordered(model_conv, jobs):
            if err:
                print('model %s failed: %s' % (os.path.basename(out), err))
            else:
                outs.append(out)
    mk = shutil.which('mkasset') or os.path.join(os.environ.get('N64_INST', '/opt/libdragon'), 'bin', 'mkasset')
    if outs and not a.no_compress:
        subprocess.run([mk, '-c', '2', '-w', '256', '-o', slashes(os.path.join(a.out, 'models'))] + [slashes(o) for o in outs], capture_output=True)
    print('models: %d, %.1f MB' % (len(outs), sum(os.path.getsize(o) for o in outs) / 1048576))


def build_nav(a, maps):
    """monster node graphs (tools/navgen.c, built here with the host compiler)"""
    here = os.path.dirname(os.path.abspath(__file__))
    # a prebuilt navgen (the Windows builder ships navgen.exe), else compile it with the host compiler
    exe = os.environ.get('HL64_NAVGEN') or shutil.which('navgen')
    if not exe:
        exe = os.path.join(tempfile.gettempdir(), 'hl64_navgen')
        subprocess.run(['cc', '-O2', '-o', exe, os.path.join(here, 'navgen.c'), '-lm'], check=True)
    n = 0
    for m in maps:
        r = subprocess.run([exe, os.path.join(a.valve, 'maps', m + '.bsp'), os.path.join(a.out, 'maps', m + '.nav')],
                           capture_output=True, text=True)
        n += r.returncode == 0
    print('node graphs: %d maps' % n)


def floor_under(bsp, p):
    """z of the first solid point (world hull 0) below p, None if p is solid or nothing within 512"""
    planes = bsp.arr('PLANES', '<4fi')
    nodes = bsp.arr('NODES', '<I2h3h3hHH')
    leafs = bsp.arr('LEAVES', '<2i6h2H4B')
    head = bsp.arr('MODELS', '<9f4i3i')[0][9]
    def contents(x, y, z):
        n = head
        while n >= 0:
            pl = planes[nodes[n][0]]
            n = nodes[n][1] if pl[0] * x + pl[1] * y + pl[2] * z - pl[3] >= 0 else nodes[n][2]
        return leafs[-n - 1][0]
    # first floor below with room for a standing player (72 units) above it: the trigger can sit in
    # a gap above an elevator's ceiling
    z, run = p[2], 0
    while z > p[2] - 1024:
        if contents(p[0], p[1], z) == -2:
            if run >= 72:
                return z + 1
            run = 0
        else:
            run += 1
        z -= 1
    return None


def build_arrivals(a, maps):
    """maps/arrive.txt: where a map is entered from the previous one, for the Debug map loader:
    '<map> <landmark> <dx> <dy> <dz> <changetarget|-> <changedelay>' with the player's spot (the
    middle of the previous map's trigger_changelevel, feet on its floor) relative to the landmark"""
    info = {}
    for m in maps:
        b = bsp2n64.BSP(open(os.path.join(a.valve, 'maps', m + '.bsp'), 'rb').read())
        ents = bsp2n64.parse_entities(b.raw('ENTITIES').split(b'\0')[0].decode('latin1'))
        info[m] = (ents, b.arr('MODELS', '<9f4i3i'), b)
    out = []
    for i, m in enumerate(maps):
        if i == 0:
            continue
        best = None
        # prefer the previous map in campaign order, then any earlier map, then any map
        order = [maps[i - 1]] + [maps[j] for j in range(i - 2, -1, -1)] + maps[i + 1:]
        for src in order:
            ents, models, bsp = info[src]
            lms = {e.get('targetname'): e for e in ents if e.get('classname') == 'info_landmark'}
            for e in ents:
                if e.get('classname') != 'trigger_changelevel' or e.get('map', '').lower() != m:
                    continue
                lm = lms.get(e.get('landmark'))
                if not lm or not e.get('model', '').startswith('*'):
                    continue
                mn = models[int(e['model'][1:])]
                lo, hi = mn[0:3], mn[3:6]
                c = [(lo[k] + hi[k]) / 2 for k in range(3)]
                # stand on the floor under the trigger (it can be a small brush up by the ceiling)
                c[2] = floor_under(bsp, c) + 37 if floor_under(bsp, c) is not None else min(c[2], lo[2] + 37)
                o = [float(v) for v in lm['origin'].split()]
                best = (e.get('landmark'), [c[k] - o[k] for k in range(3)], e.get('changetarget') or '-', e.get('changedelay') or '0')
                break
            if best:
                break
        if best:
            out.append('%s %s %.0f %.0f %.0f %s %s' % (m, best[0], *best[1], best[2], best[3]))
    open(os.path.join(a.out, 'maps', 'arrive.txt'), 'w', newline='\n').write('\n'.join(out) + '\n')
    print('arrivals: %d' % len(out))


def build_chapters(a, maps):
    """maps/chapters.txt: '<map> <chapter title>' for maps that start a chapter (worldspawn chaptertitle)"""
    titles = ui2n64.parse_titles(os.path.join(a.valve, 'titles.txt'))
    out = []
    for m in maps:
        b = bsp2n64.BSP(open(os.path.join(a.valve, 'maps', m + '.bsp'), 'rb').read())
        ents = bsp2n64.parse_entities(b.raw('ENTITIES').split(b'\0')[0].decode('latin1'))
        key = ents[0].get('chaptertitle') or ('C0A0TITLE' if m == 'c0a0' else None)
        text = titles.get(key.upper(), '').strip() if key else ''
        if text:
            out.append('%s %s' % (m, text.replace('\n', ' ')))
    open(os.path.join(a.out, 'maps', 'chapters.txt'), 'w', encoding='latin1', newline='\n').write('\n'.join(out) + '\n')
    print('chapters: %d' % len(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('valve'); ap.add_argument('out')
    ap.add_argument('--maps', nargs='*')
    ap.add_argument('--grid', type=int, default=64, help='finest lighting subdivision (texels)')
    ap.add_argument('--tol', type=float, default=12, help='lighting error allowed before subdividing')
    ap.add_argument('-j', type=int, default=os.cpu_count())
    ap.add_argument('--no-compress', action='store_true')
    ap.add_argument('--models-only', action='store_true')
    ap.add_argument('--ui-only', action='store_true')
    ap.add_argument('--maps-only', action='store_true', help='only rebuild maps + texture pack')
    ap.add_argument('--sounds-only', action='store_true', help='only rebuild sounds and music')
    ap.add_argument('--compress-level', type=int, default=3, choices=(1, 2, 3))
    a = ap.parse_args()
    maps = a.maps or [m for m in CAMPAIGN if os.path.exists(os.path.join(a.valve, 'maps', m + '.bsp'))]
    wads = [os.path.join(a.valve, w) for w in ('halflife.wad', 'liquids.wad', 'xeno.wad', 'decals.wad')]
    os.makedirs(os.path.join(a.out, 'maps'), exist_ok=True)
    if a.models_only:
        build_models(a, maps)
        return
    if a.sounds_only:
        build_sounds(a)
        return
    if a.ui_only:
        ui2n64.build(a.valve, a.out, os.path.join(tempfile.gettempdir(), 'ui2n64'))
        spr2n64.build(a.valve, a.out, maps)
        build_chapters(a, maps)
        build_arrivals(a, maps)
        return
    pack = bsp2n64.TexturePack()
    jobs = [(os.path.join(a.valve, 'maps', m + '.bsp'), wads, a.grid, a.tol) for m in maps]
    total = 0
    with Pool(a.j) as pool:
        for res in pool.imap(work, jobs):
            out = bsp2n64.write_hlb(res, os.path.join(a.out, 'maps', res['name'] + '.hlb'), pack)
            total += len(out)
    open(os.path.join(a.out, 'textures.pak'), 'wb').write(pack.data)
    # compress maps (libdragon asset compression; asset_load() decompresses on the N64).
    # Level 3 (Shrinkler) is ~20% smaller than level 2 (aPLib) and is needed to leave room on
    # the 64 MB cartridge for voices and music; it is slower to build and to load.
    mk = shutil.which('mkasset') or os.path.join(os.environ.get('N64_INST', '/opt/libdragon'), 'bin', 'mkasset')
    if not a.no_compress:
        mapdir = os.path.join(a.out, 'maps')
        files = [os.path.join(mapdir, m + '.hlb') for m in maps]
        def comp(fl):
            return subprocess.run([mk, '-c', str(a.compress_level), '-w', '256', '-o', slashes(mapdir)] + [slashes(f) for f in fl], capture_output=True)
        chunks = [files[i::a.j] for i in range(a.j)]
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(a.j) as ex:
            list(ex.map(comp, [c for c in chunks if c]))
        csize = sum(os.path.getsize(f) for f in files)
        print('compressed maps: %.1f MB' % (csize / 1048576))
    open(os.path.join(a.out, 'maps', 'maplist.txt'), 'w', newline='\n').write('\n'.join(maps) + '\n')
    build_nav(a, maps)
    shutil.copy(os.path.join(a.valve, 'skill.cfg'), os.path.join(a.out, 'skill.cfg'))
    print('maps: %d, %.1f MB; texture pack: %d textures, %.1f MB' % (
        len(maps), total / 1048576, len(pack.index), len(pack.data) / 1048576))
    if a.maps_only:
        return

    # skyboxes used by the converted maps
    skies = set()
    for m in maps:
        b = bsp2n64.BSP(open(os.path.join(a.valve, 'maps', m + '.bsp'), 'rb').read())
        ents = bsp2n64.parse_entities(b.raw('ENTITIES').split(b'\0')[0].decode('latin1'))
        skies.add((ents[0].get('skyname') or 'desert').lower())
    envdir = os.path.join(a.valve, 'gfx', 'env')
    os.makedirs(os.path.join(a.out, 'sky'), exist_ok=True)
    nsky = 0
    for sk in sorted(skies):
        try:
            sky2n64.convert(envdir, sk, os.path.join(a.out, 'sky', sk + '.sky'))
            nsky += 1
        except OSError:
            print('sky %s not found, maps using it fall back to a flat colour' % sk)
    print('skies: %d' % nsky)

    build_models(a, maps)
    ui2n64.build(a.valve, a.out, os.path.join(tempfile.gettempdir(), 'ui2n64'))
    spr2n64.build(a.valve, a.out, maps)
    build_chapters(a, maps)
    build_arrivals(a, maps)

    build_sounds(a)


# ---------------------------------------------------------------- sounds and music
# Everything is converted to libdragon's ULC codec (an RSP-decoded transform codec): it is the
# only one small enough for ~85 minutes of speech and effects plus ~55 minutes of music on a
# 64 MB cartridge. Voices keep their original 11 kHz rate; music is mono 22 kHz.
VOICE_DIRS = {'scientist', 'barney', 'vox', 'fvox', 'hgrunt', 'holo', 'gman', 'tride', 'nihilanth'}
Q_VOICE, Q_SFX, Q_MUSIC = 55, 65, 50


def wav_loop_start(path):
    """GoldSrc loops a sound from its first cue point to the end (or cue + ltxt length)"""
    import struct
    try:
        d = open(path, 'rb').read()
    except OSError:
        return None
    if d[:4] != b'RIFF' or d[8:12] != b'WAVE':
        return None
    p, cue, ltxt = 12, None, None
    while p + 8 <= len(d):
        cid, sz = d[p:p + 4], struct.unpack('<I', d[p + 4:p + 8])[0]
        body = d[p + 8:p + 8 + sz]
        if cid == b'cue ' and len(body) >= 4 and struct.unpack('<I', body[:4])[0] > 0:
            cue = struct.unpack('<I', body[4 + 20:4 + 24])[0]    # dwSampleOffset of the first point
        elif cid == b'LIST' and body[:4] == b'adtl':
            q = 4
            while q + 8 <= len(body):
                sid, ssz = body[q:q + 4], struct.unpack('<I', body[q + 4:q + 8])[0]
                if sid == b'ltxt' and ssz >= 8:
                    ltxt = struct.unpack('<I', body[q + 12:q + 16])[0]
                q += 8 + ssz + (ssz & 1)
        p += 8 + sz + (sz & 1)
    return cue


def _conv_one(job):
    conv, src, dstdir, opts = job
    os.makedirs(dstdir, exist_ok=True)
    r = subprocess.run([conv] + opts + ['-o', slashes(dstdir), slashes(src)], capture_output=True, text=True)
    base = os.path.splitext(os.path.basename(src))[0]
    out = os.path.join(dstdir, base + '.wav64')
    low = os.path.join(dstdir, base.lower() + '.wav64')
    if r.returncode == 0 and os.path.exists(out) and out != low:
        os.replace(out, low)
    return src, r.returncode, r.stderr.strip()[:300]


def build_sounds(a):
    import wave
    snd_in = os.path.join(a.valve, 'sound')
    outdir = os.path.join(a.out, 'sound')
    conv = shutil.which('audioconv64') or os.path.join(os.environ.get('N64_INST', '/opt/libdragon'), 'bin', 'audioconv64')
    os.makedirs(outdir, exist_ok=True)
    for t in ('materials.txt', 'sentences.txt'):
        if os.path.exists(os.path.join(snd_in, t)):
            shutil.copy(os.path.join(snd_in, t), os.path.join(outdir, t))
    jobs = []
    for f in sorted(glob.glob(os.path.join(snd_in, '**', '*.wav'), recursive=True)):
        rel = os.path.relpath(f, snd_in)
        top = rel.split(os.sep)[0].lower()
        try:
            w = wave.open(f)
            rate = w.getframerate()
            w.close()
        except Exception:  # noqa
            print('skipping unreadable', rel)
            continue
        q = Q_VOICE if top in VOICE_DIRS else Q_SFX
        opts = ['--wav-mono', '--wav-compress', 'ulc,quality=%d' % q]
        if rate > 22050:
            opts += ['--wav-resample', '22050']
        loop = wav_loop_start(f)
        if loop is not None:
            if rate > 22050:
                loop = loop * 22050 // rate
            opts += ['--wav-loop', 'true', '--wav-loop-offset', str(loop)]
        jobs.append((conv, f, os.path.join(outdir, os.path.dirname(rel).lower()), opts))
    media = os.path.join(a.valve, 'media')
    for f in sorted(glob.glob(os.path.join(media, '*.mp3'))):
        jobs.append((conv, f, os.path.join(a.out, 'music'),
                     ['--wav-mono', '--wav-resample', '22050', '--wav-compress', 'ulc,quality=%d' % Q_MUSIC]))
    ok = 0
    with Pool(a.j) as pool:
        for src, rc, err in pool.imap_unordered(_conv_one, jobs, chunksize=8):
            if rc == 0:
                ok += 1
            else:
                print('audioconv64 failed for', os.path.relpath(src, a.valve), err)
    def du(d):
        return sum(os.path.getsize(os.path.join(r, x)) for r, _, fs in os.walk(d) for x in fs)
    print('sounds: %d/%d converted; sound %.1f MB, music %.1f MB' % (
        ok, len(jobs), du(outdir) / 1048576, du(os.path.join(a.out, 'music')) / 1048576 if os.path.isdir(os.path.join(a.out, 'music')) else 0))


if __name__ == '__main__':
    main()
