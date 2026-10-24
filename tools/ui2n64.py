#!/usr/bin/env python3
"""
ui2n64.py - Half-Life user-interface assets for HL64.

Everything comes from the game's own files:
  gfx.wad CREDITSFONT  -> the proportional font the engine uses for HUD text messages
                          (titles.txt); rendered at three sizes for the N64's 320x240 screen
  sprites/320hud3.spr  -> 320x240 HUD sprites: train speed indicator + "HALF-LIFE" title
  resource/logo.tga    -> the main-menu logo
  titles.txt           -> HUD text messages (chapter titles, credits, Hazard Course text)

Output (filesystem/ui/):
  font_<size>.sprite + font_<size>.fnt     I4 glyph atlas + metrics ('HLF1', big-endian)
  font_medium_glow.sprite                  blurred copy of the medium font (menu highlight)
  hud3.sprite, logo.sprite                 I4 intensity images (tinted at draw time)
  titles.txt
"""
import os, sys, struct, subprocess, shutil
from PIL import Image, ImageFilter

SIZES = [('small', 0.65), ('medium', 0.8), ('large', 1.0)]
FIRST, LAST = 32, 126


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


def mksprite(png, outdir, fmt='I4'):
    tool = shutil.which('mksprite') or os.path.join(os.environ.get('N64_INST', '/opt/libdragon'), 'bin', 'mksprite')
    r = subprocess.run([tool, '-f', fmt, '-o', outdir.replace('\\', '/'), png.replace('\\', '/')], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)


def build(valve, out, tmp):
    ui = os.path.join(out, 'ui')
    os.makedirs(ui, exist_ok=True)
    os.makedirs(tmp, exist_ok=True)
    rowh, glyphs, src = read_qfont(os.path.join(valve, 'gfx.wad'))
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
    logo = Image.open(os.path.join(valve, 'resource', 'logo.tga')).convert('RGBA')
    a = logo.split()[3]
    bb = a.getbbox()
    a = a.crop(bb)
    w = 288
    a = a.resize((w, max(1, round(a.height * w / a.width))), Image.LANCZOS)
    png = os.path.join(tmp, 'logo.png')
    a.save(png)
    mksprite(png, ui)

    shutil.copy(os.path.join(valve, 'titles.txt'), os.path.join(out, 'titles.txt'))
    print('ui: fonts %s, hud sprites, logo, titles.txt' % ', '.join(n for n, _ in SIZES))


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
