#!/usr/bin/env python3
"""ps2h2h.py - the PlayStation 2 version's Head-To-Head maps and player models, from a copy of the
PS2 disc's files (the folder with SLUS_200.66, VALVE/PAK0.PAK ...), for Head-To-Head mode.

    ps2h2h.py <PS2 disc folder> <valve folder> <temp folder>

converts the ten Head-To-Head maps (plus the community maps in assets/, CUSTOM_MAPS) to PC .bsp files in <temp>/maps (tools/bs2bsp.py) and the six
characters' .dol models to <temp>/players. build_assets.py --ps2 does this and builds them into
the ROM with the rest.
"""
import os, sys, struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bs2bsp

# the PS2 front end's map list (guidata/frontend.pak guidef.txt, HeadToHeadMapSelection)
H2H_MAPS = ['basement', 'debris', 'water_canal', 'skirmish', 'waypoint', 'signal', 'office', 'snark_pit2', 'datacore2', 'stalkyard2']
# community maps that come with the port (assets/), added to the PS2 ones: name -> file
CUSTOM_MAPS = {'crimson': 'crimson.bsp'}   # "Crimson", by UnhealthyGrapejuice
# the six characters (HeadToHeadPlayerSelection: SetupPlayer1 "Gordon" "gordon", "Gina" "dm_gina" ...)
H2H_PLAYERS = [('gordon', 'pl_gordon'), ('dm_gina', 'pl_gina'), ('dm_colette', 'pl_colette'),
               ('dm_barney', 'pl_barney'), ('dm_soldier', 'pl_soldier'), ('dm_sci', 'pl_sci')]
# the guns in the players' hands (p_*.dol: the PC's p_*.mdl)
H2H_WEAPONS = ['p_crowbar', 'p_9mmhandgun', 'p_357', 'p_9mmar', 'p_crossbow', 'p_shotgun', 'p_rpg', 'p_gauss',
               'p_egon', 'p_hgun', 'p_grenade', 'p_tripmine', 'p_satchel', 'p_squeak']
# the PS2 models carry four levels of detail; split screen draws the one of about a thousand triangles
PLAYER_LOD_TRIS = 1200


def find_ci(folder, *parts):
    """a path under folder, matching each part without regard to case (the disc's names are upper case)"""
    p = folder
    for part in parts:
        try:
            names = os.listdir(p)
        except OSError:
            return None
        m = next((n for n in names if n.lower() == part.lower()), None)
        if m is None:
            return None
        p = os.path.join(p, m)
    return p


def pak0_path(ps2dir):
    return find_ci(ps2dir, 'valve', 'pak0.pak')


def pack_entries(path):
    """{name: (offset, length)} of a Quake/Half-Life PACK file"""
    with open(path, 'rb') as f:
        hdr = f.read(12)
        if hdr[:4] != b'PACK':
            raise ValueError('%s is not a PACK file' % path)
        off, ln = struct.unpack('<ii', hdr[4:12])
        f.seek(off)
        d = f.read(ln)
    out = {}
    for i in range(ln // 64):
        e = d[i * 64:i * 64 + 64]
        name = e[:56].split(b'\0')[0].decode('latin1').lower().replace('\\', '/')
        o, l = struct.unpack('<ii', e[56:64])
        out[name] = (o, l)
    return out


def read_entry(path, ent):
    with open(path, 'rb') as f:
        f.seek(ent[0])
        return f.read(ent[1])


def prepare(ps2dir, valve, tmp, log=print):
    """the maps as .bsp files ({name: path}) and the player models as .dol files ({out name: path})"""
    pak = pak0_path(ps2dir)
    if not pak:
        raise FileNotFoundError('no VALVE/PAK0.PAK in %s (choose the folder with the PS2 disc\'s files)' % ps2dir)
    ents = pack_entries(pak)
    wads = [os.path.join(valve, w) for w in ('halflife.wad', 'liquids.wad', 'xeno.wad')]
    pc = bs2bsp.wad_sizes(wads)
    os.makedirs(os.path.join(tmp, 'maps'), exist_ok=True)
    os.makedirs(os.path.join(tmp, 'players'), exist_ok=True)
    maps = {}
    for m in H2H_MAPS:
        e = ents.get('maps/%s.bs2' % m)
        if not e:
            log('Head-To-Head: %s is not on this disc' % m)
            continue
        out = os.path.join(tmp, 'maps', m + '.bsp')
        raw = read_entry(pak, e)
        data = bs2bsp.zlib.decompress(raw[4:]) if struct.unpack_from('<I', raw, 0)[0] not in (29, 30, 40) else raw
        open(out, 'wb').write(bs2bsp.convert(data, pc))
        maps[m] = out
    assets = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets')
    for m, f in CUSTOM_MAPS.items():
        p = os.path.join(assets, f)
        if os.path.exists(p): maps[m] = p
        else: log('Head-To-Head: %s is missing from assets/' % f)
    players = {}
    for src, dst in H2H_PLAYERS:
        e = ents.get('models/player/%s/%s.dol' % (src, src))
        if not e:
            log('Head-To-Head: no %s model on this disc' % src)
            continue
        out = os.path.join(tmp, 'players', src + '.dol')
        open(out, 'wb').write(read_entry(pak, e))
        players[dst] = out
    for w in H2H_WEAPONS:
        e = ents.get('models/%s.dol' % w)
        if not e:
            continue
        out = os.path.join(tmp, 'players', w + '.dol')
        open(out, 'wb').write(read_entry(pak, e))
        players[w] = out
    log('Head-To-Head: %d maps, %d characters and weapons from %s' % (len(maps), len(players), pak))
    return maps, players


if __name__ == '__main__':
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)
    m, p = prepare(sys.argv[1], sys.argv[2], sys.argv[3])
    print(m, p)
