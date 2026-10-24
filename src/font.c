// HL64 - text drawing with the fonts converted from Half-Life's gfx.wad (tools/ui2n64.py).
// Glyphs are I4 (intensity = coverage); the colour comes from the RDP primitive colour, and
// text is blended either normally (menus) or additively like the HUD text messages.
#include <libdragon.h>
#include <string.h>
#include <stdlib.h>
#include "font.h"

typedef struct { uint16_t x, y; uint8_t w, h; uint16_t adv16; } glyph_t;
typedef struct { image_t img; int height, first, count, pad; glyph_t *g; bool ok; } hlfont_t;

static hlfont_t fonts[FONT_COUNT];
static const char *names[FONT_COUNT] = {"small", "medium", "large", "medium_glow"};

static uint16_t be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

bool image_load(image_t *im, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { im->spr = NULL; return false; }
    fclose(f);
    im->spr = sprite_load(path);
    im->surf = sprite_get_pixels(im->spr);
    return true;
}

void image_draw(image_t *im, int x, int y, int sx, int sy, int w, int h)
{
    if (!im->spr) return;
    rdpq_tex_blit(&im->surf, x, y, &(rdpq_blitparms_t){.s0 = sx, .t0 = sy, .width = w, .height = h});
}

void font_init(void)
{
    for (int i = 0; i < FONT_COUNT; i++) {
        hlfont_t *fn = &fonts[i];
        char p[64];
        snprintf(p, sizeof(p), "rom:/ui/font_%s.sprite", names[i]);
        if (!image_load(&fn->img, p)) continue;
        snprintf(p, sizeof(p), "rom:/ui/font_%s.fnt", names[i]);
        int sz;
        uint8_t *d = asset_load(p, &sz);
        if (!d || memcmp(d, "HLF1", 4)) continue;
        fn->height = be16(d + 4);
        fn->first = be16(d + 6);
        fn->count = be16(d + 8);
        fn->pad = be16(d + 10);
        fn->g = malloc(sizeof(glyph_t) * fn->count);
        for (int k = 0; k < fn->count; k++) {
            const uint8_t *q = d + 12 + k * 8;
            fn->g[k] = (glyph_t){be16(q), be16(q + 2), q[4], q[5], be16(q + 6)};
        }
        free(d);
        fn->ok = true;
    }
}

int font_height(int f) { return fonts[f].ok ? fonts[f].height : 8; }

static glyph_t *glyph(hlfont_t *fn, int ch)
{
    if (ch < fn->first || ch >= fn->first + fn->count) ch = '?';
    return &fn->g[ch - fn->first];
}

int font_width(int f, const char *s)
{
    hlfont_t *fn = &fonts[f];
    if (!fn->ok) return strlen(s) * 8;
    int w16 = 0;
    for (; *s && *s != '\n'; s++) w16 += glyph(fn, (unsigned char)*s)->adv16;
    return (w16 + 8) >> 4;
}

static int cur_blend;

// The RDP blender wraps instead of saturating, so the HUD's additive text is approximated with
// an alpha blend whose opacity is the colour's brightness (identical on dark backgrounds).
void font_begin(int blend)
{
    cur_blend = blend;
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, PRIM), (TEX0, 0, PRIM, 0)));
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_filter(FILTER_POINT);
}

void font_color(color_t c)
{
    if (cur_blend == BLEND_ADD) {
        int m = c.r > c.g ? c.r : c.g;
        if (c.b > m) m = c.b;
        if (m == 0) { rdpq_set_prim_color(RGBA32(0, 0, 0, 0)); return; }
        c = RGBA32(c.r * 255 / m, c.g * 255 / m, c.b * 255 / m, m * c.a / 255);
    }
    rdpq_set_prim_color(c);
}

static int draw16(int f, int x16, int y, const char *s)
{
    hlfont_t *fn = &fonts[f];
    for (; *s && *s != '\n'; s++) {
        glyph_t *g = glyph(fn, (unsigned char)*s);
        if (*s != ' ' && g->w >= 1)
            rdpq_tex_blit(&fn->img.surf, ((x16 + 8) >> 4) - fn->pad, y - fn->pad,
                          &(rdpq_blitparms_t){.s0 = g->x, .t0 = g->y, .width = g->w, .height = g->h});
        x16 += g->adv16;
    }
    return x16;
}

int font_draw(int f, int x, int y, const char *s)
{
    if (!fonts[f].ok) { rdpq_text_printf(NULL, FONT_BUILTIN_DEBUG_MONO, x, y + 8, "%s", s); return x + strlen(s) * 8; }
    return (draw16(f, x << 4, y, s) + 8) >> 4;
}

int font_draw_char(int f, int x, int y, int ch)
{
    char s[2] = {(char)ch, 0};
    if (!fonts[f].ok) return 8;
    draw16(f, x << 4, y, s);
    return (glyph(&fonts[f], ch)->adv16 + 8) >> 4;
}

int font_draw_centered(int f, int cx, int y, const char *s) { return font_draw(f, cx - font_width(f, s) / 2, y, s); }
int font_draw_right(int f, int rx, int y, const char *s) { return font_draw(f, rx - font_width(f, s), y, s); }

// rows of a glyph that actually have ink, relative to the y passed to font_draw
bool font_ink_rows(int f, int ch, int *top, int *bottom)
{
    hlfont_t *fn = &fonts[f];
    if (!fn->ok) return false;
    glyph_t *g = glyph(fn, ch);
    surface_t *s = &fn->img.surf;
    int bpp = TEX_FORMAT_BITDEPTH(surface_get_format(s));
    int t = -1, b = -1;
    for (int r = 0; r < g->h; r++) {
        const uint8_t *row = (const uint8_t *)s->buffer + (g->y + r) * s->stride;
        for (int c = 0; c < g->w; c++) {
            int x = g->x + c, v;
            if (bpp == 4) v = (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15;
            else if (bpp == 8) v = row[x];
            else if (bpp == 16) v = row[x * 2] | row[x * 2 + 1];
            else v = row[x * 4 + 3];
            if (v) { if (t < 0) t = r; b = r; break; }
        }
    }
    if (t < 0) return false;
    *top = t - fn->pad;
    *bottom = b - fn->pad;
    return true;
}
