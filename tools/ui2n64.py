#!/usr/bin/env python3
"""
ui2n64.py - Half-Life user-interface assets for HL64.

Everything comes from the game's own files:
  gfx.wad CREDITSFONT  -> the proportional font the engine uses for HUD text messages
                          (titles.txt); rendered at three sizes for the N64's 320x240 screen
  sprites/320hud3.spr  -> 320x240 HUD sprites: train speed indicator + "HALF-LIFE" title
  resource/logo.tga    -> the main-menu logo
  titles.txt           -> HUD text messages (chapter titles, credits, Hazard Course text)
  sprites/*.spr        -> effect sprites (fx_*: explosions, muzzle flashes, beams, sparks, blood,
                          spit and energy balls), see build_fx
  assets/title.png, assets/lambda.png (this port's own art) -> title-screen background and
                          the loading icon

Output (filesystem/ui/):
  font_<size>.sprite + font_<size>.fnt     I4 glyph atlas + metrics ('HLF1', big-endian)
  font_medium_glow.sprite                  blurred copy of the medium font (menu highlight)
  hud3.sprite, logo.sprite                 I4 intensity images (tinted at draw time)
  titles.txt
"""
import os
import numpy as np, sys, struct, subprocess, shutil
from PIL import Image, ImageFilter

SIZES = [('small', 0.65), ('medium', 0.8), ('large', 1.0)]
FIRST, LAST = 32, 255   # Latin-1: the translations (valve_<language>) use its accented letters


def read_qfont(wad_path, lump='CREDITSFONT'):
    d = open(wad_path, 'rb').read()
    magic, n, ofs = struct.unpack('<4sii', d[:12])
    for i in range(n):
        fo, ds, sz, t, c, _, name = struct.unpack('<iiibbh16s', d[ofs + i * 32:ofs + i * 32 + 32])
        if name.split(b'\0')[0].decode().upper() == lump:
            b = d[fo:fo + ds]
            _, h, rowcount, rowheight = struct.unpack('<iiii', b[:16])
            info = [struct.unpack('<hh', b[16 + 4 * k:20 + 4 * k]) for k in range(256)]
            data = b[16 + 1024:16 + 1024 + 256 * h]
            img = Image.frombytes('L', (256, h), bytes(0 if p == 255 else min(255, p * 16) for p in data))
            glyphs = {}
            for ch in range(FIRST, LAST + 1):
                off, w = info[ch]
                x, y = off % 256, off // 256
                g = img.crop((x, y, x + max(w, 1), y + rowheight)) if w > 0 else Image.new('L', (1, rowheight))
                glyphs[ch] = (g, w, x, y)
            return rowheight, glyphs, img
    raise KeyError(lump)


def pack(glyphs, height, pad=1, width=256):
    """glyphs: {ch: (L image, advance)} -> atlas, [(x, y, w, h, adv)]"""
    x = y = 0
    rects = {}
    for ch in range(FIRST, LAST + 1):
        g, adv = glyphs[ch]
        if x + g.width + pad > width:
            x = 0
            y += height + pad
        rects[ch] = (x, y, g.width, g.height, adv)
        x += g.width + pad
    atlas = Image.new('L', (width, y + height + pad))
    for ch, (gx, gy, w, h, adv) in rects.items():
        atlas.paste(glyphs[ch][0], (gx, gy))
    return atlas, rects


def write_fnt(path, height, rects, glow_pad=0):
    out = bytearray(b'HLF1')
    out += struct.pack('>HHHH', height, FIRST, LAST - FIRST + 1, glow_pad)
    for ch in range(FIRST, LAST + 1):
        x, y, w, h, adv = rects[ch]
        out += struct.pack('>HHBBH', x, y, w, h, int(round(adv * 16)))
    open(path, 'wb').write(out)


def mksprite(png, outdir, fmt='I4', compress=None):
    tool = shutil.which('mksprite') or os.path.join(os.environ.get('N64_INST', '/opt/libdragon'), 'bin', 'mksprite')
    extra = ['-c', str(compress)] if compress else []
    r = subprocess.run([tool, '-f', fmt] + extra + ['-o', outdir.replace('\\', '/'), png.replace('\\', '/')], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)


def build(valve, out, tmp, h2h=False, lang=None):
    """lang: a language folder (valve_spanish ...) whose gfx.wad font and titles.txt replace valve's"""
    def lf(name):
        p = find_ci(lang, name) if lang else None
        return p or os.path.join(valve, name)
    ui = os.path.join(out, 'ui')
    os.makedirs(ui, exist_ok=True)
    os.makedirs(tmp, exist_ok=True)
    rowh, glyphs, src = read_qfont(lf('gfx.wad'))
    for name, scale in SIZES:
        h = max(1, round(rowh * scale))
        # each glyph on its own canvas (no bleed from neighbours), one spare column on the right
        sg = {}
        for ch, (g, adv, gx, gy) in glyphs.items():
            cw = max(adv, 1) + 1
            canvas = Image.new('L', (cw, rowh))
            canvas.paste(g, (0, 0))
            w = max(1, int(-(-cw * scale // 1)))
            im = canvas.resize((w, h), Image.LANCZOS) if scale != 1.0 else canvas
            if scale < 0.9:   # keep thin strokes visible at small sizes
                im = im.point(lambda v: min(255, int((v / 255.0) ** 0.75 * 255 * 1.1)))
            sg[ch] = (im, adv * scale)
        atlas, rects = pack(sg, h)
        png = os.path.join(tmp, 'font_%s.png' % name)
        atlas.save(png)
        mksprite(png, ui)
        write_fnt(os.path.join(ui, 'font_%s.fnt' % name), h, rects)
        if name == 'medium':
            # menu highlight glow (GameUI draws a blurred copy of the armed item: "MenuBlurLarge")
            gp = 3
            gg = {}
            for ch, (im, adv) in sg.items():
                big = Image.new('L', (im.width + 2 * gp, im.height + 2 * gp))
                big.paste(im, (gp, gp))
                big = big.filter(ImageFilter.GaussianBlur(1.6)).point(lambda v: min(255, v * 2))
                gg[ch] = (big, adv)
            atlas, rects = pack(gg, h + 2 * gp)
            png = os.path.join(tmp, 'font_medium_glow.png')
            atlas.save(png)
            mksprite(png, ui)
            write_fnt(os.path.join(ui, 'font_medium_glow.fnt'), h + 2 * gp, rects, gp)

    # 320x240 HUD sprites (additive, grayscale)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import spr
    _, frames = spr.read_spr(os.path.join(valve, 'sprites', '320hud3.spr'))
    hud = frames[0].convert('L')
    png = os.path.join(tmp, 'hud3.png')
    hud.save(png)
    mksprite(png, ui)
    # health / armour / ammo numbers and icons, weapon icons, weapon selection
    for n, out_name in (('320hud1', 'hud1'), ('320hud2', 'hud2')):
        _, frames = spr.read_spr(os.path.join(valve, 'sprites', n + '.spr'))
        png = os.path.join(tmp, out_name + '.png')
        frames[0].convert('L').save(png)
        mksprite(png, ui)
    # damage direction indicators (4 frames side by side: front, right, back, left)
    _, frames = spr.read_spr(os.path.join(valve, 'sprites', '320_pain.spr'))
    strip = Image.new('L', (sum(f.width for f in frames), max(f.height for f in frames)))
    x = 0
    for f in frames:
        strip.paste(f.convert('L'), (x, 0))
        x += f.width
    png = os.path.join(tmp, 'pain.png')
    strip.save(png)
    mksprite(png, ui)
    # weapon crosshairs (alpha-tested: the opaque pixels)
    _, frames = spr.read_spr(os.path.join(valve, 'sprites', 'crosshairs.spr'))
    png = os.path.join(tmp, 'crosshairs.png')
    frames[0].split()[3].save(png)
    mksprite(png, ui)

    # main menu logo: white "HALF-LIFE" with alpha -> intensity
    a = menu_logo(valve)
    bb = a.getbbox()
    a = a.crop(bb)
    w = 288
    a = a.resize((w, max(1, round(a.height * w / a.width))), Image.LANCZOS)
    png = os.path.join(tmp, 'logo.png')
    a.save(png)
    mksprite(png, ui)

    build_fx(valve, ui, tmp)
    build_title(valve, ui, tmp)
    if h2h:
        build_h2h_menu(ui, tmp)

    shutil.copy(lf('titles.txt'), os.path.join(out, 'titles.txt'))
    print('ui: fonts %s, hud sprites, logo, titles.txt' % ', '.join(n for n, _ in SIZES))



# ---------------------------------------------------------------- effect sprites
def _pixels(im):
    return list(im.get_flattened_data() if hasattr(im, 'get_flattened_data') else im.getdata())


def _norm(im):
    """additive art -> colour at full brightness, alpha = brightness (the RDP's additive blend
    overflows, so these are alpha-blended: the same look over the dark backgrounds they sit on)"""
    im = im.convert('RGB')
    out = []
    for r, g, b in _pixels(im):
        m = max(r, g, b)
        out.append((0, 0, 0, 0) if m == 0 else (r * 255 // m, g * 255 // m, b * 255 // m, m))
    o = Image.new('RGBA', im.size)
    o.putdata(out)
    return o


def _white(im):
    """white, alpha = brightest channel (tinted at draw time)"""
    im = im.convert('RGB')
    a = Image.new('L', im.size)
    a.putdata([max(p) for p in _pixels(im)])
    o = Image.new('RGBA', im.size, (255, 255, 255, 0))
    o.putalpha(a)
    return o


def find_ci(folder, *parts):
    """folder/parts..., matching each name without regard to case (None if it isn't there)"""
    p = folder
    for part in parts:
        try:
            m = next((n for n in os.listdir(p) if n.lower() == part.lower()), None)
        except OSError:
            return None
        if m is None:
            return None
        p = os.path.join(p, m)
    return p


def menu_logo(valve):
    """the main menu's "HALF-LIFE" as an alpha mask: the game's resource/logo.tga, or, for an
    install without it (some copies of the game have no resource folder), this port's own
    lettering (assets/hl.png, the lower row)"""
    p = find_ci(valve, 'resource', 'logo.tga')
    if p:
        try:
            return Image.open(p).convert('RGBA').split()[3]
        except OSError as e:
            print('ui: %s could not be read (%s): using the built-in logo' % (p, e))
    else:
        print('ui: no resource/logo.tga in %s: using the built-in logo' % valve)
    here = os.path.dirname(os.path.abspath(__file__))
    im = Image.open(os.path.join(here, '..', 'assets', 'hl.png')).convert('RGBA').convert('L')
    im = im.crop((0, 64, im.width, im.height))
    # the glow's brightness -> coverage: the letters solid, the halo faint
    return im.point(lambda v: 0 if v < 70 else min(255, (v - 70) * 4))


def build_fx(valve, ui, tmp):
    """fx_*.sprite from the game's sprites (src/weapons.c draws them as billboards and beams)"""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import spr
    sd = os.path.join(valve, 'sprites')
    names = {f.lower(): f for f in os.listdir(sd)} if os.path.isdir(sd) else {}

    def frames(n):
        f = names.get(n + '.spr')
        return spr.read_spr(os.path.join(sd, f))[1] if f else None

    made, missing = 0, []

    def out(name, im, fmt, size=None, filt=Image.LANCZOS):
        nonlocal made
        if size and im.size != size:
            im = im.resize(size, filt)
        png = os.path.join(tmp, name + '.png')
        im.save(png)
        mksprite(png, ui, fmt)
        made += 1

    jobs = [
        # name prefix, sprite, frames, recipe, format, size
        ('fx_expl', 'zerogxplode', list(range(0, 15, 2)), _norm, 'RGBA32', (32, 32)),   # 8 of its 15 frames
        ('fx_muzzle1', 'muzzleflash1', [0], _norm, 'RGBA32', (32, 32)),
        ('fx_muzzle2', 'muzzleflash2', [0], _norm, 'RGBA32', (32, 32)),
        ('fx_muzzle3', 'muzzleflash3', [0], _norm, 'RGBA32', (32, 32)),
        ('fx_xspark', 'xspark1', list(range(11)), _norm, 'RGBA32', None),              # gluon gun spark
        ('fx_xbeam', 'xbeam1', [0], _norm, 'RGBA32', None),                            # gluon gun beam
        ('fx_flare', 'flare1', [0], _white, 'RGBA32', (32, 32)),                       # env_glow fallback
        ('fx_spit', 'bigspit', [0], lambda f: f.convert('RGBA'), 'RGBA32', None),      # bullsquid
        ('fx_mspit', 'mommaspit', [0], lambda f: f.convert('RGBA'), 'RGBA32', None),   # gonarch
        ('fx_nball', 'nhth1', [0], _norm, 'RGBA32', (32, 32)),                         # nihilanth zap
        ('fx_cball', 'xspark4', [0], _norm, 'RGBA32', None),                           # controller ball
        ('fx_tball', 'exit1', [0], _norm, 'RGBA32', (32, 32)),                         # teleport ball
        ('fx_blood', 'blood', [0], lambda f: f.convert('RGBA'), 'IA8', (32, 32), Image.BILINEAR),
        ('fx_wallpuff', 'wallpuff', [0], _norm, 'IA8', (32, 32), Image.BILINEAR),
        # beams, as on the PC: the sprite's width runs across the beam, its height along it
        ('fx_lgtning', 'lgtning', [0], lambda f: f.convert('L'), 'I8', None),
        ('fx_laserbeam', 'laserbeam', [0], lambda f: f.convert('L'), 'I8', None),
    ]
    for job in jobs:
        name, src, idx, recipe, fmt, size = job[:6]
        filt = job[6] if len(job) > 6 else Image.LANCZOS
        fr = frames(src)
        if not fr:
            missing.append(src)
            continue
        many = len(idx) > 1
        for k, i in enumerate(idx):
            out('%s%d' % (name, k) if many else name, recipe(fr[min(i, len(fr) - 1)]), fmt, size, filt)
    # decals (decals.wad): the palette index is the coverage, palette[255] the colour (tinted in
    # the game); kept as white + alpha. dc_shot0..4, dc_bigshot0..4, dc_scorch0..2, dc_blood0..7, dc_yblood0..5
    wad = os.path.join(valve, 'decals.wad')
    if os.path.exists(wad):
        import bsp2n64
        w = bsp2n64.read_wads([wad])
        for base, cnt, size in (('shot', 5, 16), ('bigshot', 5, 16), ('scorch', 3, 32), ('blood', 8, 32), ('yblood', 6, 32)):
            for k in range(cnt):
                blob = w.get('{%s%d' % (base, k + 1))
                if blob is None:
                    continue
                _, dw, dh, pix, pal = bsp2n64.parse_miptex(blob)
                a = Image.fromarray(np.asarray(pix, np.uint8).reshape(dh, dw), 'L')
                im = Image.merge('LA', (Image.new('L', (dw, dh), 255), a))
                out('dc_%s%d' % (base, k), im, 'IA8', (size, size) if (dw, dh) != (size, size) else None, Image.BILINEAR)
    print('effect sprites: %d%s' % (made, (', missing: ' + ' '.join(missing)) if missing else ''))


# ---------------------------------------------------------------- title background

def build_h2h_menu(ui, tmp):
    """the Head-To-Head menu's art (assets/: the community build's menu): its logo, the
    characters' portraits and faces, the map previews and thumbnails. Only built with the PS2 maps."""
    here = os.path.dirname(os.path.abspath(__file__))
    h2h_src = os.path.join(here, '..', 'assets', 'head2head_logo.png')
    if os.path.exists(h2h_src):
        h2h_im = Image.open(h2h_src).convert('RGB')
        top = h2h_im.crop((0, 0, 512, 64))
        bot = h2h_im.crop((0, 64, 512, 128))

        p_top = Image.new('RGBA', top.size)
        for y in range(top.height):
            for x in range(top.width):
                r, g, b = top.getpixel((x, y))
                br = max(r, g, b)
                if br <= 4:
                    p_top.putpixel((x, y), (0, 0, 0, 0))
                else:
                    a = int(min(110, max(0, (br - 4) / (80.0 - 4.0) * 110)))
                    scale = 255.0 / max(1, min(255, int((br / 94.0) * 255)))
                    p_top.putpixel((x, y), (min(255, int(r * scale)), min(255, int(g * scale)), min(255, int(b * scale)), a))

        p_bot = Image.new('RGBA', bot.size)
        for y in range(bot.height):
            for x in range(bot.width):
                r, g, b = bot.getpixel((x, y))
                br = max(r, g, b)
                if br <= 4:
                    p_bot.putpixel((x, y), (0, 0, 0, 0))
                else:
                    a = int(min(255, max(0, (br - 4) / (85.0 - 4.0) * 255)))
                    scale = 255.0 / max(1, a)
                    p_bot.putpixel((x, y), (min(255, int(r * scale)), min(255, int(g * scale)), min(255, int(b * scale)), a))

        h2h_top_png = os.path.join(tmp, 'h2h_top.png')
        h2h_bot_png = os.path.join(tmp, 'h2h_bot.png')
        p_top.save(h2h_top_png)
        p_bot.save(h2h_bot_png)
        mksprite(h2h_top_png, ui, 'RGBA32', 2)
        mksprite(h2h_bot_png, ui, 'RGBA32', 2)

    for c in ('gina', 'gordon', 'colette', 'barney', 'decay_soldier', 'decay_scientist'):
        c_src = os.path.join(here, '..', 'assets', c + '.png')
        if os.path.exists(c_src):
            c_im = Image.open(c_src).convert('RGBA')
            c_png = os.path.join(tmp, c + '.png')
            c_im.save(c_png)
            mksprite(c_png, ui, 'RGBA16', 2)

    h2h_sel_src = os.path.join(here, '..', 'assets', 'h2h_select.png')
    if os.path.exists(h2h_sel_src):
        h2h_sel = Image.open(h2h_sel_src).convert('RGBA')
        faces = [
            ('h2h_face0', (10, 3, 57, 61)),
            ('h2h_face1', (10, 67, 57, 125)),
            ('h2h_face2', (74, 67, 121, 125)),
            ('h2h_face3', (74, 3, 121, 61)),
            ('h2h_face4', (74, 131, 121, 189)),
            ('h2h_face5', (10, 131, 57, 189)),
            ('h2h_p1', (7, 192, 60, 256)),
            ('h2h_p2', (73, 192, 122, 256)),
        ]
        for name, box in faces:
            crop = h2h_sel.crop(box)
            crop_png = os.path.join(tmp, name + '.png')
            crop.save(crop_png)
            mksprite(crop_png, ui, 'RGBA32', 2)

    # Head-to-head map previews (h2h_pieces_01.png and h2h_pieces_02.png)
    h2h_map_crops = [
        ('h2h_map0', 'h2h_pieces_01.png', (0, 0, 128, 64)),
        ('h2h_map1', 'h2h_pieces_01.png', (128, 0, 256, 64)),
        ('h2h_map2', 'h2h_pieces_01.png', (0, 64, 128, 128)),
        ('h2h_map3', 'h2h_pieces_01.png', (128, 64, 256, 128)),
        ('h2h_map4', 'h2h_pieces_01.png', (0, 128, 128, 192)),
        ('h2h_map5', 'h2h_pieces_01.png', (128, 128, 256, 192)),
        ('h2h_map6', 'h2h_pieces_01.png', (0, 192, 128, 256)),
        ('h2h_map7', 'h2h_pieces_01.png', (128, 192, 256, 256)),
        ('h2h_map8', 'h2h_pieces_02.png', (0, 0, 128, 64)),
        ('h2h_map9', 'h2h_pieces_02.png', (128, 0, 256, 64)),
        ('h2h_map10', 'h2h_pieces_crimson.png', (0, 0, 128, 64)),
    ]
    for mname, msrc, mbox in h2h_map_crops:
        mpath = os.path.join(here, '..', 'assets', msrc)
        if os.path.exists(mpath):
            mim = Image.open(mpath).convert('RGBA').crop(mbox)
            mpng = os.path.join(tmp, mname + '.png')
            mim.save(mpng)
            mksprite(mpng, ui, 'RGBA16', 2)

    # Head-to-head map thumbnails (h2h_map_selections.png)
    h2h_sel_path = os.path.join(here, '..', 'assets', 'h2h_map_selections.png')
    if os.path.exists(h2h_sel_path):
        thumb_im = Image.open(h2h_sel_path).convert('RGBA')
        thumbs = [
            ('h2h_thumb0', (2, 5, 72, 36)),
            ('h2h_thumb1', (144, 5, 214, 36)),
            ('h2h_thumb2', (73, 39, 143, 70)),
            ('h2h_thumb3', (2, 73, 72, 104)),
            ('h2h_thumb4', (144, 73, 214, 104)),
            ('h2h_thumb5', (73, 107, 143, 138)),
            ('h2h_thumb6', (2, 141, 72, 172)),
            ('h2h_thumb7', (144, 141, 214, 172)),
            ('h2h_thumb8', (73, 175, 143, 206)),
            ('h2h_thumb9', (2, 209, 72, 240)),
            ('h2h_thumb_hi', (156, 211, 230, 238)),
        ]
        for tname, tbox in thumbs:
            tim = thumb_im.crop(tbox)
            tpng = os.path.join(tmp, tname + '.png')
            tim.save(tpng)
            mksprite(tpng, ui, 'RGBA16', 2)

    h2h_crimson_thumb = os.path.join(here, '..', 'assets', 'h2h_map_selections_crimson.png')
    if os.path.exists(h2h_crimson_thumb):
        cthumb_im = Image.open(h2h_crimson_thumb).convert('RGBA').crop((2, 5, 72, 36))
        cpng = os.path.join(tmp, 'h2h_thumb10.png')
        cthumb_im.save(cpng)
        mksprite(cpng, ui, 'RGBA16', 2)

    # Arrow right (arrows_02.png)
    arrow_path = os.path.join(here, '..', 'assets', 'arrows_02.png')
    if os.path.exists(arrow_path):
        aim = Image.open(arrow_path).convert('RGBA').crop((16, 0, 32, 32))
        apng = os.path.join(tmp, 'h2h_arrow_r.png')
        aim.save(apng)
        mksprite(apng, ui, 'RGBA32', 2)

def build_title(valve, ui, tmp):
    """title.sprite (320x240 title-screen background) and lambda.sprite (the icon beside
    "LOADING"): this port's own artwork, kept in the repository's assets/ folder"""
    here = os.path.dirname(os.path.abspath(__file__))
    for name in ('title', 'lambda', 'glowcircle', 'menuglow', 'buttons_a', 'buttons_b',
                 'color2', 'colorstrip2', 'slider', 'slider_end_cap', 'slider_piece', 'box'):
        src = os.path.join(here, '..', 'assets', name + '.png')
        if not os.path.exists(src):
            continue
        im = Image.open(src).convert('RGBA')
        if name == 'lambda':   # drawn at this size beside "LOADING"
            im = im.resize((24, 24), Image.LANCZOS)
        png = os.path.join(tmp, name + '.png')
        im.save(png)
        fmt = 'RGBA16' if name == 'title' else 'RGBA32'
        mksprite(png, ui, fmt)

    # the N64 pad's button icons (assets/buttons: A, B, Z, L, R, Start, the D-pad, C buttons and
    # stick directions), shown in place of the bindings' names in the menus and the Hazard Course
    bdir = os.path.join(here, '..', 'assets', 'buttons')
    if os.path.isdir(bdir):
        for f in sorted(os.listdir(bdir)):
            if not f.lower().endswith('.png'):
                continue
            im = Image.open(os.path.join(bdir, f)).convert('RGBA').resize((32, 32), Image.LANCZOS)
            png = os.path.join(tmp, 'btn_' + f.lower())
            im.save(png)
            mksprite(png, ui, 'RGBA32', 2)

    ctrl_src = os.path.join(here, '..', 'assets', 'ctrl.png')
    if os.path.exists(ctrl_src):
        ctrl_im = Image.open(ctrl_src).convert('RGBA')
        ctrl_small = ctrl_im.resize((188, 145), Image.LANCZOS)
        ctrl_sharp = ctrl_small.filter(ImageFilter.UnsharpMask(radius=0.8, percent=150, threshold=2))
        ctrl_png = os.path.join(tmp, 'ctrl.png')
        ctrl_sharp.save(ctrl_png)
        mksprite(ctrl_png, ui, 'RGBA32')

    hl_src = os.path.join(here, '..', 'assets', 'hl.png')
    if os.path.exists(hl_src):
        hl_im = Image.open(hl_src).convert('RGB')
        top = hl_im.crop((0, 0, 512, 64))
        bot = hl_im.crop((0, 64, 512, 128))

        p_top = Image.new('RGBA', top.size)
        for y in range(top.height):
            for x in range(top.width):
                r, g, b = top.getpixel((x, y))
                br = max(r, g, b)
                if br <= 4:
                    p_top.putpixel((x, y), (0, 0, 0, 0))
                else:
                    a = int(min(110, max(0, (br - 4) / (80.0 - 4.0) * 110)))
                    scale = 255.0 / max(1, min(255, int((br / 94.0) * 255)))
                    p_top.putpixel((x, y), (min(255, int(r * scale)), min(255, int(g * scale)), min(255, int(b * scale)), a))

        p_bot = Image.new('RGBA', bot.size)
        for y in range(bot.height):
            for x in range(bot.width):
                r, g, b = bot.getpixel((x, y))
                br = max(r, g, b)
                if br <= 4:
                    p_bot.putpixel((x, y), (0, 0, 0, 0))
                else:
                    a = int(min(255, max(0, (br - 4) / (85.0 - 4.0) * 255)))
                    scale = 255.0 / max(1, a)
                    p_bot.putpixel((x, y), (min(255, int(r * scale)), min(255, int(g * scale)), min(255, int(b * scale)), a))

        top_png = os.path.join(tmp, 'hl_top.png')
        bot_png = os.path.join(tmp, 'hl_bot.png')
        p_top.save(top_png)
        p_bot.save(bot_png)
        mksprite(top_png, ui, 'RGBA32')
        mksprite(bot_png, ui, 'RGBA32')
    print('title background and loading icon: assets/')


def parse_titles(path):
    """titles.txt -> {NAME: text}"""
    msgs = {}
    lines = open(path, encoding='latin1').read().replace('\r', '').split('\n')
    i = 0
    while i < len(lines):
        l = lines[i].strip()
        if l and not l.startswith('//') and not l.startswith('$') and i + 1 < len(lines) and lines[i + 1].strip() == '{':
            j = i + 2
            body = []
            while j < len(lines) and lines[j].strip() != '}':
                body.append(lines[j])
                j += 1
            msgs[l.upper()] = '\n'.join(body)
            i = j
        i += 1
    return msgs


if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else os.path.join(__import__('tempfile').gettempdir(), 'ui2n64'))
