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
import re, struct

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


def asset_size_ok(path):
    """a compressed asset's header agrees with its file size (as libdragon's asset_read_header checks)"""
    d = open(path, 'rb').read(17)
    if d[:3] != b'DCA':
        return False
    pos, vals = 5, []
    for _ in range(3):   # cmp_size, orig_size, inplace_margin (LEB128)
        v = sh = 0
        while True:
            b = d[pos]; pos += 1
            v |= (b & 0x7f) << sh; sh += 7
            if not b & 0x80:
                break
        vals.append(v)
    return vals[0] + pos + (pos & 1) == os.path.getsize(path)


def compress_files(level, files, jobs=1):
    """libdragon asset compression of files, in place. mkasset writes into a separate folder and
    the results then replace the originals: compressing a file onto itself left the old file's
    tail after the compressed data on Windows (asset_load then fails: "Wrong compressed size").
    Each result is checked against its header; a file that doesn't match stays uncompressed."""
    import struct as _st
    from concurrent.futures import ThreadPoolExecutor
    mk = shutil.which('mkasset') or os.path.join(os.environ.get('N64_INST', '/opt/libdragon'), 'bin', 'mkasset')
    files = list(files)
    if not files:
        return
    tmp = tempfile.mkdtemp(prefix='hl64_mkasset_', dir=os.path.dirname(os.path.abspath(files[0])))
    try:
        chunks = [files[i::jobs] for i in range(jobs)]
        def run(fl):
            # (a few files per call keeps the command line well under Windows' limit)
            for i in range(0, len(fl), 40):
                subprocess.run([mk, '-c', str(level), '-w', '256', '-o', slashes(tmp)] + [slashes(f) for f in fl[i:i + 40]], capture_output=True)
        with ThreadPoolExecutor(jobs) as ex:
            list(ex.map(run, [c for c in chunks if c]))
        bad = 0
        for f in files:
            c = os.path.join(tmp, os.path.basename(f))
            try:
                ok = asset_size_ok(c)
            except OSError:
                ok = False
            if ok:
                os.replace(c, f)
            else:
                bad += 1
        if bad:
            print('compression: %d files left uncompressed (mkasset output didn\'t check out)' % bad)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


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
        b = bsp2n64.BSP(open(bsp2n64.map_path(a.valve, m), 'rb').read())
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
    # turrets: off / deploy / fire / spin / retire / die (turret.cpp plays them by name)
    for t in ('turret', 'miniturret', 'sentry'):
        if t in used:
            used[t].add('*')
    # tentacles (Blast Pit) idle and strike at the level of their target (CTentacle::MyLevel)
    if 'tentacle2' in used:
        used['tentacle2'] |= {'pit_idle', 'rise_to_temp1', 'temp1_to_floor',   # out of the pit and back
                              'floor_to_lev1', 'lev1_to_lev2', 'lev2_to_lev3'}   # rising and sinking between levels
        for lv in ('floor', 'lev1', 'lev2', 'lev3'):      # CTentacle::Cycle: idle, strike, tap, look behind
            used['tentacle2'] |= {lv + '_idle', lv + '_strike', lv + '_tap', lv + '_rear_idle'}
        # the rocket engine burns them (CommandUse / DieThink)
        used['tentacle2'] |= {'engine_idle', 'engine_sway', 'engine_swat', 'engine_bob', 'engine_death1'}
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
    if outs and not a.no_compress:
        compress_files(2, outs, a.j)
    print('models: %d, %.1f MB' % (len(outs), sum(os.path.getsize(o) for o in outs) / 1048576))


def build_players(a, players):
    """Head-To-Head characters: the PS2 player models (.dol), every sequence (the weapon aims and
    shots, gaits, deaths), the level of detail of about a thousand triangles"""
    if not players:
        return
    import ps2h2h
    os.makedirs(os.path.join(a.out, 'models'), exist_ok=True)
    outs = []
    for name, path in sorted(players.items()):
        out = os.path.join(a.out, 'models', name + '.hlm')
        try:
            mdl2n64.convert(path, out, ['*'], verbose=False, lod=ps2h2h.PLAYER_LOD_TRIS, hand_root=name.startswith('p_'))
            outs.append(out)
        except Exception as e:  # noqa
            print('player model %s failed: %r' % (name, e))
    if outs and not a.no_compress:
        compress_files(3, outs, a.j)
    print('Head-To-Head characters: %d, %.1f MB' % (len(outs), sum(os.path.getsize(o) for o in outs) / 1048576))


def build_nav(a, maps):
    """monster node graphs (tools/navgen.c, built here with the host compiler)"""
    here = os.path.dirname(os.path.abspath(__file__))
    # a prebuilt navgen (the Windows builder ships navgen.exe), else compile it with the host compiler
    exe = os.environ.get('HL64_NAVGEN') or shutil.which('navgen') or (os.path.join(here, 'navgen.exe') if os.path.exists(os.path.join(here, 'navgen.exe')) else None)
    if not exe:
        exe = os.path.join(tempfile.gettempdir(), 'hl64_navgen')
        subprocess.run(['cc', '-O2', '-o', exe, os.path.join(here, 'navgen.c'), '-lm'], check=True)
    n = 0
    for m in maps:
        r = subprocess.run([exe, bsp2n64.map_path(a.valve, m), os.path.join(a.out, 'maps', m + '.nav')],
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
        b = bsp2n64.BSP(open(bsp2n64.map_path(a.valve, m), 'rb').read())
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
                # fired by name (c4a1f's portal): the player is wherever the map put him for the
                # transition, which is the trigger_transition volume named after the landmark
                o = [float(v) for v in lm['origin'].split()]
                far = max(abs((mn[k] + mn[k + 3]) / 2 - o[k]) for k in range(3)) > 512
                if e.get('targetname') and far:
                    tv = next((t for t in ents if t.get('classname') == 'trigger_transition' and
                               t.get('targetname') == e.get('landmark') and t.get('model', '').startswith('*')), None)
                    if tv:
                        mn = models[int(tv['model'][1:])]
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
    titles = ui2n64.parse_titles(ui2n64.find_ci(a.lang_text_dir, 'titles.txt') if a.lang_text_dir else os.path.join(a.valve, 'titles.txt'))
    out = []
    for m in maps:
        b = bsp2n64.BSP(open(bsp2n64.map_path(a.valve, m), 'rb').read())
        ents = bsp2n64.parse_entities(b.raw('ENTITIES').split(b'\0')[0].decode('latin1'))
        key = ents[0].get('chaptertitle') or ('C0A0TITLE' if m == 'c0a0' else None)
        text = titles.get(key.upper(), '').strip() if key else ''
        if text:
            out.append('%s %s' % (m, text.replace('\n', ' ')))
    open(os.path.join(a.out, 'maps', 'chapters.txt'), 'w', encoding='latin1', newline='\n').write('\n'.join(out) + '\n')
    print('chapters: %d' % len(out))


# files the build can't do without: checked first, so a folder that lacks one stops the build
# straight away (and says what's missing) instead of an hour in, after the maps
REQUIRED = [('maps', 'c1a0.bsp'), ('halflife.wad',), ('gfx.wad',), ('titles.txt',), ('skill.cfg',),
            ('sprites', '320hud1.spr'), ('sprites', '320hud2.spr'), ('sprites', '320hud3.spr'),
            ('sprites', '320_pain.spr'), ('sprites', 'crosshairs.spr'), ('sound',), ('models',)]


def preflight(valve):
    import ui2n64
    missing = ['/'.join(r) for r in REQUIRED if not ui2n64.find_ci(valve, *r)]
    if missing:
        sys.exit('This Half-Life folder is missing files the build needs:\n  %s\n(%s)\n'
                 'Choose the "valve" folder of a full Half-Life install (Steam: steamapps/common/Half-Life/valve).'
                 % ('\n  '.join(missing), valve))


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
    ap.add_argument('--lang', help='the game language to take speech and text from: a Steam language '
                    '(spanish, french, german ...) whose valve_<language> folder is next to valve, or that folder')
    ap.add_argument('--ps2', help='a copy of the PS2 disc\'s files: adds the Head-To-Head maps and characters')
    a = ap.parse_args()
    preflight(a.valve)
    a.lang_dir = language_dir(a.valve, a.lang)
    # the on-screen text too, except for the languages the one-byte font can't show
    a.lang_text_dir = a.lang_dir if a.lang_dir and not any(
        k in os.path.basename(a.lang_dir).lower() for k in NONLATIN) else None
    if a.lang_dir:
        print('language: %s (%s)' % (a.lang_dir, 'speech and text' if a.lang_text_dir else 'speech; the text stays English'))
    maps = a.maps or [m for m in CAMPAIGN if os.path.exists(bsp2n64.map_path(a.valve, m))]
    # the PS2 version's Head-To-Head maps (converted to .bsp in a temporary folder) go through
    # the same conversion as the campaign's, sharing its texture pack
    h2h_maps, h2h_players = {}, {}
    if a.ps2:
        import ps2h2h
        h2h_maps, h2h_players = ps2h2h.prepare(a.ps2, a.valve, os.path.join(tempfile.gettempdir(), 'hl64_ps2'))
        bsp2n64.EXTRA_MAPS.update(h2h_maps)
    allmaps = maps + [m for m in h2h_maps if m not in maps]
    wads = [os.path.join(a.valve, w) for w in ('halflife.wad', 'liquids.wad', 'xeno.wad', 'decals.wad')]
    os.makedirs(os.path.join(a.out, 'maps'), exist_ok=True)
    if a.models_only:
        build_models(a, allmaps)
        build_players(a, h2h_players)
        return
    if a.sounds_only:
        build_sounds(a)
        return
    if a.ui_only:
        ui2n64.build(a.valve, a.out, os.path.join(tempfile.gettempdir(), 'ui2n64'), h2h=bool(a.ps2), lang=a.lang_text_dir)
        spr2n64.build(a.valve, a.out, allmaps)
        build_chapters(a, maps)
        build_arrivals(a, maps)
        return
    pack = bsp2n64.TexturePack()
    jobs = [(bsp2n64.map_path(a.valve, m), wads, a.grid, a.tol) for m in allmaps]
    total = 0
    with Pool(a.j) as pool:
        for res in pool.imap(work, jobs):
            out = bsp2n64.write_hlb(res, os.path.join(a.out, 'maps', res['name'] + '.hlb'), pack)
            total += len(out)
    open(os.path.join(a.out, 'textures.pak'), 'wb').write(pack.data)
    # compress maps (libdragon asset compression; asset_load() decompresses on the N64).
    # Level 3 (Shrinkler) is ~20% smaller than level 2 (aPLib) and is needed to leave room on
    # the 64 MB cartridge for voices and music; it is slower to build and to load.
    if not a.no_compress:
        mapdir = os.path.join(a.out, 'maps')
        files = [os.path.join(mapdir, m + '.hlb') for m in allmaps]
        compress_files(a.compress_level, files, a.j)
        csize = sum(os.path.getsize(f) for f in files)
        print('compressed maps: %.1f MB' % (csize / 1048576))
    open(os.path.join(a.out, 'maps', 'maplist.txt'), 'w', newline='\n').write('\n'.join(maps) + '\n')
    build_nav(a, maps)
    shutil.copy(os.path.join(a.valve, 'skill.cfg'), os.path.join(a.out, 'skill.cfg'))
    print('maps: %d (%d Head-To-Head), %.1f MB; texture pack: %d textures, %.1f MB' % (
        len(allmaps), len(allmaps) - len(maps), total / 1048576, len(pack.index), len(pack.data) / 1048576))
    if a.maps_only:
        return

    # skyboxes used by the converted maps
    skies = set()
    for m in allmaps:
        b = bsp2n64.BSP(open(bsp2n64.map_path(a.valve, m), 'rb').read())
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

    build_models(a, allmaps)
    build_players(a, h2h_players)
    ui2n64.build(a.valve, a.out, os.path.join(tempfile.gettempdir(), 'ui2n64'), h2h=bool(a.ps2), lang=a.lang_text_dir)
    spr2n64.build(a.valve, a.out, allmaps)
    build_chapters(a, maps)
    build_arrivals(a, maps)

    build_sounds(a)


# ---------------------------------------------------------------- sounds and music
# Everything is converted to libdragon's ULC codec (an RSP-decoded transform codec): it is the
# only one small enough for ~85 minutes of speech and effects plus ~55 minutes of music on a
# 64 MB cartridge. Voices keep their original 11 kHz rate; music is mono 22 kHz.
VOICE_DIRS = {'scientist', 'barney', 'vox', 'fvox', 'hgrunt', 'holo', 'gman', 'tride', 'nihilanth', 'sentences'}
Q_VOICE, Q_SFX, Q_MUSIC = 55, 65, 50
Q_SENTENCE = 45
ROM_OVERHEAD = 1280 * 1024   # the engine, symbols and filesystem headers on top of the converted files, with a margin


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
        # via a temporary name: on a case-insensitive filesystem (Windows) a direct rename that
        # only changes case can do nothing
        os.replace(out, low + '.tmp')
        os.replace(low + '.tmp', low)
    return src, r.returncode, r.stderr.strip()[:300]


# Steam languages written with more than one byte a letter: the HUD font (one byte a letter, from the
# language's own gfx.wad) can't show them, so their text stays English. The others' titles.txt use
# their own code page, which their own font matches
NONLATIN = ('japanese', 'koreana', 'schinese', 'tchinese', 'thai', 'vietnamese')


def language_dir(valve, lang):
    """the folder of a Steam game language (valve_spanish ...), or None for English"""
    if not lang or lang.lower() in ('english', 'none', ''):
        return None
    if os.path.isdir(lang):
        return lang
    d = ui2n64.find_ci(os.path.dirname(os.path.abspath(valve)), 'valve_' + lang.lower())
    if not d:
        raise FileNotFoundError('no valve_%s folder next to %s: set Half-Life to that language in Steam '
                                '(Properties -> General -> Language) so Steam downloads it' % (lang.lower(), valve))
    return d


def lang_file(a, *parts):
    """a file of the game, from the language folder when it has its own copy"""
    p = ui2n64.find_ci(a.lang_dir, *parts) if getattr(a, 'lang_dir', None) else None
    return p or os.path.join(a.valve, *parts)


def sentence_files(path):
    """the sound files (lower case, relative to sound/) a sentences.txt uses (sound.c sentence_words)"""
    used = set()
    for line in open(path, encoding='latin-1', errors='replace'):
        line = line.split('//')[0].strip()
        parts = line.split(None, 1)
        if len(parts) < 2:
            continue
        text = re.sub(r'\([^)]*\)', ' ', parts[1])
        first = text.split()[0] if text.split() else ''
        d = 'vox/'
        if '/' in first:
            d = first[:first.rfind('/') + 1]
            text = text[text.find(first) + len(d):]
        for w in re.split(r'[\s,.]+', text):
            if w:
                used.add(((w if '/' in w else d + w) + '.wav').lower())
    return used


# the word libraries the English sentences are built from; a translation records whole sentences
# (sound/sentences/), so with a language only the words still used are kept
WORD_DIRS = ('vox', 'fvox', 'hgrunt')
CODE_SOUNDS = ('hgrunt/gr_',)   # played by the engine itself (npc.c: the soldiers' pain, death, gun)


def map_sounds(valve):
    """every .wav the maps' entities name (ambient_generic messages, button and door sounds ...)"""
    used = set()
    for f in glob.glob(os.path.join(valve, 'maps', '*.bsp')):
        try:
            d = open(f, 'rb').read()
            o, l = struct.unpack_from('<ii', d, 4)
            for m in re.findall(r'"([^"]+\.wav)"', d[o:o + l].decode('latin-1')):
                used.add(m.replace('\\', '/').lower().lstrip('*!'))
        except (OSError, struct.error):
            pass
    return used


def build_sounds(a):
    import wave
    snd_in = os.path.join(a.valve, 'sound')
    outdir = os.path.join(a.out, 'sound')
    conv = shutil.which('audioconv64') or os.path.join(os.environ.get('N64_INST', '/opt/libdragon'), 'bin', 'audioconv64')
    os.makedirs(outdir, exist_ok=True)
    for t in ('materials.txt', 'sentences.txt'):
        src = lang_file(a, 'sound', t)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(outdir, t))
    # the sounds: valve's, with the language folder's in place of (and besides) them. With a
    # language, the announcer's and the suit's word libraries (vox/, fvox/) keep only the words its
    # sentences.txt still uses: the translations record whole sentences instead (sound/sentences/)
    files = {}
    for f in glob.glob(os.path.join(snd_in, '**', '*.wav'), recursive=True):
        files[os.path.relpath(f, snd_in).replace(os.sep, '/').lower()] = f
    lang_snd = ui2n64.find_ci(a.lang_dir, 'sound') if getattr(a, 'lang_dir', None) else None
    if lang_snd:
        n = 0
        for f in glob.glob(os.path.join(lang_snd, '**', '*.wav'), recursive=True):
            files[os.path.relpath(f, lang_snd).replace(os.sep, '/').lower()] = f
            n += 1
        used = sentence_files(os.path.join(outdir, 'sentences.txt')) | map_sounds(a.valve)
        drop = [k for k in files if k.split('/')[0] in WORD_DIRS and k not in used
                and not any(k.startswith(c) for c in CODE_SOUNDS)]
        for k in drop:
            del files[k]
        print('language: %d sounds of its own, %d unused announcer/suit words left out' % (n, len(drop)))
    jobs = []
    for rel_l, f in sorted(files.items()):
        rel = rel_l.replace('/', os.sep)
        top = rel.split(os.sep)[0].lower()
        try:
            w = wave.open(f)
            rate = w.getframerate()
            w.close()
        except Exception:  # noqa
            print('skipping unreadable', rel)
            continue
        q = Q_VOICE if top in VOICE_DIRS else Q_SFX
        if top == 'sentences':
            q = Q_SENTENCE   # a translation's recorded announcements: the room they take decides if it fits
        opts = ['--wav-mono', '--wav-compress', 'ulc,quality=%d' % q]
        if rate > 22050:
            opts += ['--wav-resample', '22050']
        loop = wav_loop_start(f)
        if loop is not None:
            if rate > 22050:
                loop = loop * 22050 // rate
            opts += ['--wav-loop', 'true', '--wav-loop-offset', str(loop)]
        jobs.append((conv, f, os.path.join(outdir, os.path.dirname(rel).lower()), opts))
    assets_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets')
    for snd in ('select', 'confirm', 'cancel'):
        src = os.path.join(assets_dir, snd + '.mp3')
        if os.path.exists(src):
            jobs.append((conv, src, os.path.join(outdir, 'ui'), ['--wav-mono', '--wav-compress', 'ulc,quality=%d' % Q_SFX]))
    # the title screen's music (this port's own, assets/FELOOP.mp3 -> music/feloop.wav64)
    fe = os.path.join(assets_dir, 'FELOOP.mp3')
    if os.path.exists(fe):
        jobs.append((conv, fe, os.path.join(a.out, 'music'), ['--wav-mono', '--wav-resample', '22050', '--wav-compress', 'ulc,quality=%d' % Q_MUSIC]))
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
    fit_budget(a, jobs, lang_snd, du)


def fit_budget(a, jobs, lang_snd, du):
    """Everything has to fit a 64 MB cartridge (the ROM is the converted files plus about 1 MB of
    engine). A translation, a mod or custom maps can take more room than the game alone: while the
    data is over, re-encode sounds harder, step by step: a translation's speech first, then all
    speech, effects and music, lower each round"""
    budget = int(os.environ.get('HL64_FS_BUDGET', 64 * 1048576 - ROM_OVERHEAD))   # (the variable: tests)
    if du(a.out) <= budget:
        return

    def group(j):
        src = os.path.abspath(j[1])
        if src.lower().endswith('.mp3') or os.path.basename(j[2]).lower() == 'music':
            return 'music'
        if lang_snd and src.startswith(os.path.abspath(lang_snd)):
            top = os.path.relpath(src, lang_snd).replace(os.sep, '/').split('/')[0].lower()
            return 'lang' if top in VOICE_DIRS else 'sfx'
        top = os.path.relpath(src, os.path.join(a.valve, 'sound')).replace(os.sep, '/').split('/')[0].lower()
        return 'voice' if top in VOICE_DIRS else 'sfx'
    groups = {}
    for jb in jobs:
        groups.setdefault(group(jb), []).append(jb)
    steps = [('lang', 40), ('lang', 30), ('voice', 40), ('sfx', 50), ('music', 38), ('lang', 22), ('voice', 30),
             ('sfx', 38), ('music', 28), ('lang', 15), ('voice', 22), ('sfx', 28), ('music', 20), ('voice', 15), ('sfx', 18),
             ('music', 12), ('lang', 10), ('voice', 10), ('sfx', 10)]
    for g, q in steps:
        total = du(a.out)
        if total <= budget:
            break
        if not groups.get(g):
            continue
        print('size: the game data is %.2f MB, over the %.2f MB that fit: compressing %s harder (quality %d)' % (
            total / 1048576, budget / 1048576, {'lang': 'the translation\'s speech', 'voice': 'speech', 'sfx': 'sound effects', 'music': 'music'}[g], q))
        redo = [(c, src, d, [o if not o.startswith('ulc,') else 'ulc,quality=%d' % q for o in opts]) for c, src, d, opts in groups[g]]
        with Pool(a.j) as pool:
            for src, rc, err in pool.imap_unordered(_conv_one, redo, chunksize=8):
                if rc != 0:
                    print('audioconv64 failed for', src, err)
    total = du(a.out)
    print('size: game data %.2f MB of %.2f MB' % (total / 1048576, budget / 1048576))
    if total > budget:   # never make a ROM bigger than a cartridge
        sys.exit('The game data doesn\'t fit a 64 MB cartridge even with the sounds compressed as far as they go: '
                 'build it without Head-To-Head.')


if __name__ == '__main__':
    main()
