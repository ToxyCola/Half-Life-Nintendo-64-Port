// navgen - build a node graph for HL64 from a Half-Life map's info_node entities.
//
// The PC game builds this at map load (dlls/nodes.cpp CGraph::LinkVisibleNodes / TestRoute):
// two nodes are linked when a monster of a given hull can walk from one to the other with
// MOVE_STEP (18-unit steps, never off a ledge). The N64 is too slow to do that at load time,
// so it is done here, against the map's own clipping hulls, and stored next to the map.
//
//   cc -O2 -o navgen tools/navgen.c -lm
//   navgen <map.bsp> <out.nav>
//
// Output (big-endian): "HLN1" u16 numnodes u16 numlinks
//   node: f32 x,y,z (on the floor for ground nodes), u8 flags (1 = air), u8 pad, u16 firstlink, u16 numlinks, u16 pad
//   link: u16 dest, u8 hulls (bit h = walkable with BSP hull h), u8 pad, i16 door (brush model through which it passes, -1 none), u16 pad
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

typedef struct { float n[3], d; int type; } plane_t;
typedef struct { int planenum; short ch[2]; } clipnode_t;
typedef struct { float mins[3], maxs[3], org[3]; int head[4]; int vis, ff, nf; } model_t;

static uint8_t *bsp;
static plane_t *planes; static int nplanes;
static clipnode_t *clip; static int nclip;
static model_t *models; static int nmodels;
static char *ents;

static const float hull_mins[4][3] = {{0, 0, 0}, {-16, -16, -36}, {-32, -32, -32}, {-16, -16, -18}};

#define CONTENTS_SOLID -2
#define DIST_EPSILON 0.03125f

static int g_head;
typedef struct { float frac; float end[3]; int allsolid, startsolid, inopen; float normal[3]; } trace_t;

static int point_contents(int num, const float *p)
{
    while (num >= 0) {
        clipnode_t *c = &clip[num];
        plane_t *pl = &planes[c->planenum];
        float d = pl->type < 3 ? p[pl->type] - pl->d : pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - pl->d;
        num = c->ch[d < 0];
    }
    return num;
}

// Quake's SV_RecursiveHullCheck
static int recursive(int num, float p1f, float p2f, const float *p1, const float *p2, trace_t *tr)
{
    if (num < 0) {
        if (num != CONTENTS_SOLID) { tr->allsolid = 0; tr->inopen = 1; }
        else tr->startsolid = 1;
        return 1;
    }
    clipnode_t *c = &clip[num];
    plane_t *pl = &planes[c->planenum];
    float t1, t2;
    if (pl->type < 3) { t1 = p1[pl->type] - pl->d; t2 = p2[pl->type] - pl->d; }
    else {
        t1 = pl->n[0] * p1[0] + pl->n[1] * p1[1] + pl->n[2] * p1[2] - pl->d;
        t2 = pl->n[0] * p2[0] + pl->n[1] * p2[1] + pl->n[2] * p2[2] - pl->d;
    }
    if (t1 >= 0 && t2 >= 0) return recursive(c->ch[0], p1f, p2f, p1, p2, tr);
    if (t1 < 0 && t2 < 0) return recursive(c->ch[1], p1f, p2f, p1, p2, tr);
    float frac = t1 < 0 ? (t1 + DIST_EPSILON) / (t1 - t2) : (t1 - DIST_EPSILON) / (t1 - t2);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    float midf = p1f + (p2f - p1f) * frac, mid[3];
    for (int i = 0; i < 3; i++) mid[i] = p1[i] + frac * (p2[i] - p1[i]);
    int side = t1 < 0;
    if (!recursive(c->ch[side], p1f, midf, p1, mid, tr)) return 0;
    if (point_contents(c->ch[side ^ 1], mid) != CONTENTS_SOLID) return recursive(c->ch[side ^ 1], midf, p2f, mid, p2, tr);
    if (tr->allsolid) return 0;
    if (!side) { memcpy(tr->normal, pl->n, 12); }
    else { for (int i = 0; i < 3; i++) tr->normal[i] = -pl->n[i]; }
    // back off until the mid point is outside (as in Quake)
    while (point_contents(g_head, mid) == CONTENTS_SOLID) {
        frac -= 0.1f;
        if (frac < 0) { tr->frac = midf; memcpy(tr->end, mid, 12); return 0; }
        midf = p1f + (p2f - p1f) * frac;
        for (int i = 0; i < 3; i++) mid[i] = p1[i] + frac * (p2[i] - p1[i]);
    }
    tr->frac = midf;
    memcpy(tr->end, mid, 12);
    return 0;
}


static void trace_model(int model, int hull, const float *org, const float *a, const float *b, trace_t *tr)
{
    memset(tr, 0, sizeof(*tr));
    tr->frac = 1;
    tr->allsolid = 1;
    float s[3], e[3];
    for (int i = 0; i < 3; i++) { s[i] = a[i] - org[i]; e[i] = b[i] - org[i]; }
    g_head = models[model].head[hull];
    recursive(g_head, 0, 1, s, e, tr);
    if (tr->frac == 1) memcpy(tr->end, b, 12);
    else for (int i = 0; i < 3; i++) tr->end[i] = a[i] + tr->frac * (b[i] - a[i]);
}

// ---------------------------------------------------------------- entities
typedef struct { char cls[48]; float org[3]; int model; int spawnflags; char tn[48]; } ent_t;
static ent_t E[4096];
static int nE;

static void parse_ents(void)
{
    char *p = ents;
    while ((p = strchr(p, '{'))) {
        ent_t *e = &E[nE];
        memset(e, 0, sizeof(*e));
        e->model = -1;
        p++;
        while (*p && *p != '}') {
            char *k = strchr(p, '"');
            if (!k || k > strchr(p, '}')) break;
            char *ke = strchr(k + 1, '"');
            char *v = strchr(ke + 1, '"');
            char *ve = strchr(v + 1, '"');
            char key[64] = {0}, val[256] = {0};
            memcpy(key, k + 1, ke - k - 1 < 63 ? ke - k - 1 : 63);
            memcpy(val, v + 1, ve - v - 1 < 255 ? ve - v - 1 : 255);
            if (!strcmp(key, "classname")) strncpy(e->cls, val, 47);
            else if (!strcmp(key, "targetname")) strncpy(e->tn, val, 47);
            else if (!strcmp(key, "origin")) sscanf(val, "%f %f %f", &e->org[0], &e->org[1], &e->org[2]);
            else if (!strcmp(key, "model") && val[0] == '*') e->model = atoi(val + 1);
            else if (!strcmp(key, "spawnflags")) e->spawnflags = atoi(val);
            p = ve + 1;
        }
        if (nE < 4095) nE++;
        p = strchr(p, '}');
        if (!p) break;
    }
}

static int is_door(ent_t *e) { return !strcmp(e->cls, "func_door") || !strcmp(e->cls, "func_door_rotating"); }
static int is_blocker(ent_t *e)
{
    if (e->model <= 0) return 0;
    if (!strcmp(e->cls, "func_wall") || !strcmp(e->cls, "func_breakable") || !strcmp(e->cls, "func_pushable") ||
        !strcmp(e->cls, "func_monsterclip") || !strcmp(e->cls, "func_guntarget") || !strcmp(e->cls, "func_tank"))
        return 1;
    if (!strcmp(e->cls, "func_wall_toggle") && !(e->spawnflags & 1)) return 1;
    return 0;
}

// world + blocking brush entities; returns the door crossed (or -1) through *door
static void trace_all(int hull, const float *a, const float *b, trace_t *tr, int *door)
{
    float zero[3] = {0, 0, 0};
    trace_model(0, hull, zero, a, b, tr);
    for (int i = 0; i < nE; i++) {
        ent_t *e = &E[i];
        int d = is_door(e);
        if (!d && !is_blocker(e)) continue;
        if (e->model <= 0 || e->model >= nmodels) continue;
        model_t *m = &models[e->model];
        // bounding box reject (rotating doors are placed at their origin)
        int out = 0;
        for (int k = 0; k < 3; k++) {
            float lo = fminf(a[k], b[k]) - 40, hi = fmaxf(a[k], b[k]) + 40;
            if (hi < m->mins[k] + e->org[k] - 64 || lo > m->maxs[k] + e->org[k] + 64) out = 1;
        }
        if (out) continue;
        trace_t t;
        trace_model(e->model, hull, e->org, a, b, &t);
        if (t.frac < 1 || t.startsolid) {
            if (d) { if (door) *door = e->model; continue; }
            if (t.frac < tr->frac || t.startsolid) { *tr = t; }
        }
    }
}

static float hull_ofs(int hull) { return -hull_mins[hull][2]; }   // feet -> hull centre

// MOVE_STEP walk from a to b (feet positions) for a hull
static int walk(int hull, const float *a, const float *b, int *door)
{
    float pos[3] = {a[0], a[1], a[2] + hull_ofs(hull)};
    float dx = b[0] - a[0], dy = b[1] - a[1];
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist < 1) return fabsf(a[2] - b[2]) < 24;
    dx /= dist; dy /= dist;
    float moved = 0;
    trace_t tr;
    // starting position must be free
    trace_all(hull, pos, pos, &tr, NULL);
    if (tr.startsolid) {
        pos[2] += 2;
        trace_all(hull, pos, pos, &tr, NULL);
        if (tr.startsolid) return 0;
    }
    while (moved < dist - 0.5f) {
        float st = dist - moved > 16 ? 16 : dist - moved;
        float up[3] = {pos[0], pos[1], pos[2] + 18};
        float fw[3] = {pos[0] + dx * st, pos[1] + dy * st, pos[2] + 18};
        trace_t t1;
        trace_all(hull, up, up, &t1, NULL);
        if (t1.startsolid) { up[2] = pos[2]; fw[2] = pos[2]; }
        trace_all(hull, up, fw, &t1, door);
        if (t1.startsolid || t1.frac < 1) return 0;
        float dn[3] = {t1.end[0], t1.end[1], t1.end[2] - 18 - 18};
        trace_t t2;
        trace_all(hull, t1.end, dn, &t2, door);
        if (t2.allsolid || t2.startsolid) return 0;
        if (t2.frac >= 1) return 0;               // ledge: MOVE_STEP won't step off
        if (t2.normal[2] < 0.7f) return 0;        // too steep
        memcpy(pos, t2.end, 12);
        moved += st;
    }
    float fz = pos[2] - hull_ofs(hull);
    return fabsf(fz - b[2]) < 24;
}

static int fly(int hull, const float *a, const float *b)
{
    float s[3] = {a[0], a[1], a[2]}, e[3] = {b[0], b[1], b[2]};
    trace_t tr;
    trace_all(hull, s, e, &tr, NULL);
    return !tr.startsolid && tr.frac >= 1;
}

// ---------------------------------------------------------------- graph
#define MAXN 2048
typedef struct { float o[3]; int air; } node_t;
typedef struct { int a, b; int hulls; int door; float d; int dead; } link_t;
static node_t N[MAXN];
static int nN;
static link_t *L;
static int nL, capL;

static void wbe32(FILE *f, uint32_t v) { uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v}; fwrite(b, 1, 4, f); }
static void wbe16(FILE *f, uint16_t v) { uint8_t b[2] = {v >> 8, v}; fwrite(b, 1, 2, f); }
static void wf(FILE *f, float x) { uint32_t v; memcpy(&v, &x, 4); wbe32(f, v); }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: navgen map.bsp out.nav\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    bsp = malloc(sz + 1);
    fread(bsp, 1, sz, f);
    fclose(f);
    int32_t *lump = (int32_t *)(bsp + 4);
    ents = (char *)bsp + lump[0];
    bsp[lump[0] + lump[1] - 1] = 0;
    nplanes = lump[3] / 20;
    planes = malloc(sizeof(plane_t) * nplanes);
    for (int i = 0; i < nplanes; i++) memcpy(&planes[i], bsp + lump[2] + i * 20, 20);
    nclip = lump[19] / 8;
    clip = (clipnode_t *)(bsp + lump[18]);
    nmodels = lump[29] / 64;
    models = (model_t *)(bsp + lump[28]);
    parse_ents();

    for (int i = 0; i < nE && nN < MAXN; i++) {
        int air = !strcmp(E[i].cls, "info_node_air");
        if (strcmp(E[i].cls, "info_node") && !air) continue;
        node_t *n = &N[nN++];
        memcpy(n->o, E[i].org, 12);
        n->air = air;
        if (!air) {
            // drop to the floor with the human hull (CGraph: DROP_TO_FLOOR)
            float a[3] = {n->o[0], n->o[1], n->o[2] + 36 + 8}, b[3] = {n->o[0], n->o[1], n->o[2] - 384 + 36};
            trace_t tr;
            float zero[3] = {0, 0, 0};
            trace_model(0, 1, zero, a, b, &tr);
            if (!tr.startsolid && tr.frac < 1) n->o[2] = tr.end[2] - 36;
            else {
                trace_model(0, 3, zero, (float[3]){a[0], a[1], n->o[2] + 18 + 8}, (float[3]){a[0], a[1], n->o[2] - 384 + 18}, &tr);
                if (!tr.startsolid && tr.frac < 1) n->o[2] = tr.end[2] - 18;
            }
        }
    }
    capL = 1024;
    L = malloc(sizeof(link_t) * capL);
    for (int i = 0; i < nN; i++) {
        for (int j = i + 1; j < nN; j++) {
            if (N[i].air != N[j].air) continue;
            float d[3] = {N[j].o[0] - N[i].o[0], N[j].o[1] - N[i].o[1], N[j].o[2] - N[i].o[2]};
            float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dist > 1200) continue;
            int hulls = 0, door = -1;
            if (N[i].air) {
                if (fly(3, N[i].o, N[j].o)) hulls |= 1 << 3;
                if (fly(1, N[i].o, N[j].o)) hulls |= 1 << 1;
                if (fly(2, N[i].o, N[j].o)) hulls |= 1 << 2;
            } else {
                // cheap line of sight first (small hull at knee height)
                float a[3] = {N[i].o[0], N[i].o[1], N[i].o[2] + 18 + 18}, b[3] = {N[j].o[0], N[j].o[1], N[j].o[2] + 18 + 18};
                trace_t tr;
                int dd = -1;
                trace_all(3, a, b, &tr, &dd);
                if (tr.startsolid || tr.frac < 1) {
                    a[2] += 18; b[2] += 18;
                    trace_all(3, a, b, &tr, &dd);
                    if (tr.startsolid || tr.frac < 1) continue;
                }
                int dd1 = -1, dd2 = -1, dd3 = -1;
                if (walk(3, N[i].o, N[j].o, &dd3) && walk(3, N[j].o, N[i].o, NULL)) hulls |= 1 << 3;
                if ((hulls & 8) && walk(1, N[i].o, N[j].o, &dd1) && walk(1, N[j].o, N[i].o, NULL)) hulls |= 1 << 1;
                if ((hulls & 2) && walk(2, N[i].o, N[j].o, &dd2) && walk(2, N[j].o, N[i].o, NULL)) hulls |= 1 << 2;
                door = dd1 >= 0 ? dd1 : (dd3 >= 0 ? dd3 : dd2);
            }
            if (!hulls) continue;
            if (nL + 2 > capL) { capL *= 2; L = realloc(L, sizeof(link_t) * capL); }
            L[nL++] = (link_t){i, j, hulls, door, dist, 0};
            L[nL++] = (link_t){j, i, hulls, door, dist, 0};
        }
    }
    // RejectInlineLinks: drop a->c when a->b->c runs the same way and b is closer
    for (int x = 0; x < nL; x++) {
        link_t *ac = &L[x];
        float dac[3] = {N[ac->b].o[0] - N[ac->a].o[0], N[ac->b].o[1] - N[ac->a].o[1], N[ac->b].o[2] - N[ac->a].o[2]};
        for (int y = 0; y < nL && !ac->dead; y++) {
            link_t *ab = &L[y];
            if (y == x || ab->a != ac->a || ab->dead || ab->d >= ac->d) continue;
            float dab[3] = {N[ab->b].o[0] - N[ab->a].o[0], N[ab->b].o[1] - N[ab->a].o[1], N[ab->b].o[2] - N[ab->a].o[2]};
            float dot = (dac[0] * dab[0] + dac[1] * dab[1] + dac[2] * dab[2]) / (ac->d * ab->d);
            if (dot < 0.995f) continue;
            // b must reach c with at least the same hulls
            for (int z = 0; z < nL; z++)
                if (!L[z].dead && L[z].a == ab->b && L[z].b == ac->b && (L[z].hulls & ac->hulls) == ac->hulls) { ac->dead = 1; break; }
        }
    }
    // write, links grouped by source node
    FILE *o = fopen(argv[2], "wb");
    int live = 0;
    for (int x = 0; x < nL; x++) live += !L[x].dead;
    fwrite("HLN1", 1, 4, o);
    wbe16(o, nN);
    wbe16(o, live);
    int first = 0;
    for (int i = 0; i < nN; i++) {
        int c = 0;
        for (int x = 0; x < nL; x++) if (!L[x].dead && L[x].a == i) c++;
        wf(o, N[i].o[0]); wf(o, N[i].o[1]); wf(o, N[i].o[2]);
        fputc(N[i].air, o); fputc(0, o);
        wbe16(o, first); wbe16(o, c); wbe16(o, 0);
        first += c;
    }
    for (int i = 0; i < nN; i++)
        for (int x = 0; x < nL; x++) {
            if (L[x].dead || L[x].a != i) continue;
            wbe16(o, L[x].b);
            fputc(L[x].hulls, o); fputc(0, o);
            wbe16(o, (uint16_t)(int16_t)L[x].door);
            wbe16(o, 0);
        }
    fclose(o);
    printf("%s: %d nodes, %d links\n", argv[1], nN, live);
    return 0;
}
