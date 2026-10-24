"""env_sprite / env_glow / cycler_sprite art: the PC's .spr files -> N64 frame strips.

Every sprite the campaign's maps reference is converted if its .spr is in valve/sprites; one
that is missing is pointed at the closest one that exists (e.g. the G-Man's E-Tele1 portal at
the Xen exit1 portal), so dropping the full PC sprites folder in and rebuilding gives the exact art.

Output:
  spr/<name>.sprite   all frames stacked vertically (frame 0 on top)
  spr/index.txt       one line per referenced name:
                      name file frames fw fh orig_w orig_h type texfmt fmt tint_r tint_g tint_b spr_frames
                      (frames: in the strip; spr_frames: in the .spr, which sets the timing)

Formats (the RDP's additive blend overflows, so additive art is drawn as alpha blending with
alpha = brightness, which looks the same over the dark backgrounds these effects sit on):
  I8      grey (or single-hue) art: intensity = alpha, tinted with tint_rgb at draw time, <= 64x64
  RGBA32  coloured art: colour normalised to full brightness, alpha = brightness, <= 40x40 and 12 frames,
          drawn in 32x32 tiles (one TMEM upload each)
  SPR_INDEXALPHA art (smoke, decals) -> I8 of the index (alpha), tint = the palette's last colour.
  SPR_ALPHTEST art -> RGBA32 with 0/255 alpha.
"""
import os, sys, struct, shutil, subprocess
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spr

MAX_FRAMES = 16
MAX_FRAMES_RGBA = 12   # full-colour strips cost 4 bytes a texel of RAM


def pixels(im):
    return list(im.get_flattened_data() if hasattr(im, 'get_flattened_data') else im.getdata())
CLASSES = ('env_sprite', 'env_glow', 'cycler_sprite')
# sprites the game code itself uses (nihilanth.cpp: exit1 teleport balls, nhth1 zaps;
# controller.cpp: xspark4 energy balls)
CODE_SPRITES = ('exit1', 'nhth1', 'xspark4')

# a referenced sprite that is not on disk -> the nearest one that usually is (in order)
SUBSTITUTE = {
    'e-tele1': ['exit1'], 'b-tele1': ['exit1'], 'tele1': ['exit1'], 'enter1': ['exit1'], 'portal1': ['exit1'],
    'xffloor': ['glow01', 'flare1'], 'xflare1': ['flare1'], 'xflare2': ['flare1'], 'xflare3': ['flare1'],
    'flare2': ['flare1'], 'flare3': ['flare1'], 'flare6': ['flare1'], 'blueflare1': ['flare1'],
    'yelflare1': ['flare1'], 'hotglow': ['glow01', 'flare1'],
    'glow02': ['glow01'], 'glow03': ['glow01'], 'glow04': ['glow01'], 'glow05': ['glow01'],
    'fexplo': ['zerogxplode'], 'fexplo1': ['zerogxplode'], 'eexplo': ['zerogxplode'], 'cexplo': ['zerogxplode'],
    'gexplo': ['zerogxplode'], 'dexplo': ['zerogxplode'], 'explode1': ['zerogxplode'], 'blast': ['zerogxplode'],
    'mushroom': ['zerogxplode'],
    'xspark2': ['xspark4', 'xspark1'], 'xspark3': ['xspark4', 'xspark1'],
    'muz8': ['muzzleflash2'], 'rjet1': ['muzzleflash3', 'muzzleflash2'],
    'xsmoke1': ['smoke'], 'xsmoke3': ['smoke'], 'xsmoke4': ['smoke'], 'xssmke1': ['smoke'], 'steam1': ['smoke'],
    'ballsmoke': ['smoke'], 'stmbal1': ['smoke'],
}


def referenced(valve, maps):
    import bsp2n64
    names = set()
    for m in maps:
        b = bsp2n64.BSP(open(os.path.join(valve, 'maps', m + '.bsp'), 'rb').read())
        for e in bsp2n64.parse_entities(b.raw('ENTITIES').split(b'\0')[0].decode('latin1')):
            if e.get('classname', '').lower() in CLASSES:
                mdl = e.get('model', '').lower().replace('\\', '/')
                if mdl.endswith('.spr'):
                    names.add(os.path.splitext(os.path.basename(mdl))[0])
    return names


def find_spr(valve, name):
    d = os.path.join(valve, 'sprites')
    if not os.path.isdir(d):
        return None
    for f in os.listdir(d):
        if f.lower() == name + '.spr':
            return os.path.join(d, f)
    return None


def header(path):
    d = open(path, 'rb').read(40)
    _, _, typ, texfmt, _, w, h, n, _, _ = struct.unpack('<4siiifiiifi', d)
    return typ, texfmt, w, h, n


def single_hue(frames):
    """(True, tint) when the art is one colour at varying brightness"""
    tot = [0, 0, 0]
    px = []
    for f in frames:
        for r, g, b, a in pixels(f.convert('RGBA')):
            if r + g + b >= 48:
                px.append((r, g, b))
                tot[0] += r; tot[1] += g; tot[2] += b
    if not px:
        return True, (255, 255, 255)
    s = float(sum(tot))
    avg = [t / s for t in tot]
    dev = sum(sum(abs(c / float(r + g + b) - avg[k]) for k, c in enumerate((r, g, b))) for r, g, b in px) / len(px)
    m = max(avg)
    tint = tuple(int(round(255 * a / m)) for a in avg)
    return dev < 0.15, tint


def fit(w, h, cap):
    s = min(1.0, float(cap) / max(w, h))
    fw = max(2, int(round(w * s / 2)) * 2)
    fh = max(2, int(round(h * s / 2)) * 2)
    return fw, fh


def convert(path, name, outdir, tmp):
    typ, texfmt, ow, oh, n = header(path)
    _, frames = spr.read_spr(path)
    if len(frames) > MAX_FRAMES:   # keep the animation's length, drop frames evenly
        frames = [frames[int(i * len(frames) / MAX_FRAMES)] for i in range(MAX_FRAMES)]
    tint = (255, 255, 255)
    if texfmt == 2:                                  # SPR_INDEXALPHA: alpha in the index
        fmt = 'I8'
        r, g, b, _ = frames[0].getpixel((0, 0))
        # read_spr puts the palette colour of each index in rgb; the art's colour is the top entry
        tint = max((f.convert('RGBA').getpixel((x, y))[:3] for f in frames[:1]
                    for x in range(f.width) for y in range(f.height)), key=lambda c: sum(c))
        conv = [f.split()[3] for f in frames]
    elif texfmt == 3:                                # SPR_ALPHTEST
        fmt = 'RGBA32'
        conv = [f.convert('RGBA') for f in frames]
    else:                                            # normal / additive: brightness = alpha
        mono, t = single_hue(frames)
        if mono:
            fmt, tint = 'I8', t
            conv = [f.convert('RGB').convert('L') for f in frames]
        else:
            fmt = 'RGBA32'
            conv = []
            for f in frames:
                src = f.convert('RGB')
                im = Image.new('RGBA', src.size)
                out = []
                for r, g, b in pixels(src):
                    m = max(r, g, b)
                    if m == 0:
                        out.append((0, 0, 0, 0))
                    else:
                        out.append((r * 255 // m, g * 255 // m, b * 255 // m, m))
                im.putdata(out)
                conv.append(im)
    if fmt != 'I8' and len(conv) > MAX_FRAMES_RGBA:
        conv = [conv[int(i * len(conv) / MAX_FRAMES_RGBA)] for i in range(MAX_FRAMES_RGBA)]
    fw, fh = fit(conv[0].width, conv[0].height, 64 if fmt == 'I8' else 40)
    strip = Image.new(conv[0].mode, (fw, fh * len(conv)))
    for i, c in enumerate(conv):
        strip.paste(c.resize((fw, fh), Image.LANCZOS), (0, i * fh))
    png = os.path.join(tmp, name + '.png')
    strip.save(png)
    tool = shutil.which('mksprite') or os.path.join(os.environ.get('N64_INST', '/opt/libdragon'), 'bin', 'mksprite')
    r = subprocess.run([tool, '-f', fmt, '-c', '1', '-o', outdir.replace('\\', '/'), png.replace('\\', '/')], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return len(conv), fw, fh, ow, oh, typ, texfmt, fmt, tint, n


def build(valve, out, maps, tmp=None):
    import tempfile
    tmp = tmp or os.path.join(tempfile.gettempdir(), 'spr2n64')
    outdir = os.path.join(out, 'spr')
    os.makedirs(outdir, exist_ok=True)
    os.makedirs(tmp, exist_ok=True)
    names = referenced(valve, maps) | set(CODE_SPRITES)
    done = {}
    lines, missing = [], []
    for name in sorted(names):
        src = name if find_spr(valve, name) else None
        if not src:
            src = next((s for s in SUBSTITUTE.get(name, []) if find_spr(valve, s)), None)
            if not src:
                missing.append(name)
                continue
        if src not in done:
            done[src] = convert(find_spr(valve, src), src, outdir, tmp)
        nf, fw, fh, ow, oh, typ, texfmt, fmt, tint, srcn = done[src]
        lines.append('%s %s %d %d %d %d %d %d %d %s %d %d %d %d' % (name, src, nf, fw, fh, ow, oh, typ, texfmt, fmt, tint[0], tint[1], tint[2], srcn))
    open(os.path.join(outdir, 'index.txt'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    size = sum(os.path.getsize(os.path.join(outdir, s + '.sprite')) for s in done)
    print('env sprites: %d referenced, %d converted (%d KB), %d substituted, %d not drawn: %s' % (
        len(names), len(done), size // 1024, sum(1 for l in lines if l.split()[0] != l.split()[1]), len(missing), ' '.join(missing)))


if __name__ == '__main__':
    valve, out = sys.argv[1], sys.argv[2]
    import build_assets
    maps = [m for m in build_assets.CAMPAIGN if os.path.exists(os.path.join(valve, 'maps', m + '.bsp'))]
    build(valve, out, maps)
