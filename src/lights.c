// Light styles (CLight / LIGHT_STYLE): the map converter bakes lighting into vertex colours and
// keeps, for every vertex lit by a switchable (32..63) or animated (1..11) style, its base light
// and the light each style adds. Here the styles are evaluated and those vertex colours rebuilt
// whenever a style's brightness changes (light switches, flickering lights).
#include <libdragon.h>
#include <string.h>
#include "bsp.h"
#include "game.h"

static const char *presets[12] = {
    "m", "mmnmmommommnonmmonqnmmo", "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba",
    "mmmmmaaaaammmmmaaaaaabcdefgabcdefg", "mamamamamama", "jklmnopqrstuvwxyzyxwvutsrqponmlkj",
    "nmonqnmomnmomomno", "mmmaaaabcdefgmmmmaaaammmaamm", "mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa",
    "aaaaaaaazzzzzzzz", "mmamammmmammamamaaamammma", "abcdefghijklmnopqrrqponmlkjihgfedcba"};
static float applied[64];
static bool on[64];
static float anim_t;

static float pattern_value(const char *p, float t)
{
    int n = strlen(p);
    if (n == 0) return 1;
    int i = (int)(t * 10) % n;   // 10 frames a second
    return (p[i] - 'a') / (float)('m' - 'a');
}

static float style_value(int s)
{
    if (s == 0) return 1;
    if (s < 12) return pattern_value(presets[s], anim_t);
    if (s < 32) return 1;
    return on[s] ? 1 : 0;
}

void lights_map_start(void)
{
    anim_t = 0;
    for (int s = 0; s < 64; s++) { on[s] = true; applied[s] = -1; }
    for (int i = 1; i < num_entities; i++) {
        entity_t *e = &entities[i];
        if (strncmp(e->classname, "light", 5)) continue;
        const char *st = ent_value(e, "style");
        if (!st) continue;
        int s = atoi(st);
        if (s >= 32 && s < 64) on[s] = !(e->spawnflags & 1);   // SF_LIGHT_START_OFF
    }
}

// CLight::Use: a named light with a switchable style toggles it
bool lights_use(entity_t *e)
{
    if (strncmp(e->classname, "light", 5)) return false;
    const char *st = ent_value(e, "style");
    if (!st) return true;
    int s = atoi(st);
    if (s >= 32 && s < 64) on[s] = !on[s];
    return true;
}

static inline uint32_t clamp_light(int v) { return v > 1023 ? 1023 : (v < 0 ? 0 : v); }

void lights_think(float dt)
{
    anim_t += dt;
    if (!world.lightstyles || world.numlightstyles <= 0) return;
    float val[64];
    bool changed[64], any = false;
    for (int s = 0; s < 64; s++) {
        val[s] = style_value(s);
        changed[s] = val[s] != applied[s];
        any |= changed[s];
    }
    if (!any) return;
    const uint8_t *gamma = world.lightstyles;
    const uint32_t *r = (const uint32_t *)(world.lightstyles + 1024);
    uint8_t *vb = (uint8_t *)world.verts;
    int nupd = 0;
    for (int k = 0; k < world.numlightstyles; k++) {
        uint32_t w0 = r[0];
        bool two = w0 >> 31;
        int vi = w0 & 0x7ffff, s0 = (w0 >> 19) & 63, s1 = (w0 >> 25) & 63;
        uint32_t base = r[1], c0 = r[2], c1 = two ? r[3] : 0;
        r += two ? 4 : 3;
        if (!changed[s0] && !(two && changed[s1])) continue;
        nupd++;
        float v0 = val[s0], v1 = two ? val[s1] : 0;
        int rr = (base >> 20 & 1023) + (int)((c0 >> 20 & 1023) * v0) + (int)((c1 >> 20 & 1023) * v1);
        int gg = (base >> 10 & 1023) + (int)((c0 >> 10 & 1023) * v0) + (int)((c1 >> 10 & 1023) * v1);
        int bb = (base & 1023) + (int)((c0 & 1023) * v0) + (int)((c1 & 1023) * v1);
        uint8_t *col = vb + (vi >> 1) * 32 + 16 + (vi & 1) * 4;
        col[0] = gamma[clamp_light(rr)];
        col[1] = gamma[clamp_light(gg)];
        col[2] = gamma[clamp_light(bb)];
        data_cache_hit_writeback(col, 4);
    }
    for (int s = 32; s < 64; s++) if (changed[s] && applied[s] >= 0) debugf("lightstyle %d -> %.0f (%d vertices relit)\n", s, val[s], nupd);
    for (int s = 0; s < 64; s++) applied[s] = val[s];
}
