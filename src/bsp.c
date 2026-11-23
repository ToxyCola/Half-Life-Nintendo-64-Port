// HL64 - map loading, PVS and hull collision for the N64.
#include <libdragon.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include "bsp.h"

world_t world;

static const float hull_sizes[4][2][3] = {
    {{0, 0, 0}, {0, 0, 0}},
    {{-16, -16, -36}, {16, 16, 36}},
    {{-32, -32, -32}, {32, 32, 32}},
    {{-16, -16, -18}, {16, 16, 18}},
};

void bsp_free(void)
{
    if (world.base) free(world.base);
    if (world.texblock) free(world.texblock);
    if (world.models) free(world.models);
    memset(&world, 0, sizeof(world));
}

int bsp_load_serial;   // bumped by every map load (caches keyed on the map)
bool bsp_load(const char *mapname)
{
    bsp_free();
    char path[64];
    snprintf(path, sizeof(path), "rom:/maps/%s.hlb", mapname);
    FILE *f = fopen(path, "rb");
    if (!f) { debugf("bsp_load: cannot open %s\n", path); return false; }
    fclose(f);
    // maps are aPLib-compressed in the ROM (mkasset -c 2); asset_load decompresses
    int size = 0;
    uint64_t t0 = get_ticks_ms();
    uint8_t *buf = asset_load(path, &size);
    if (!buf) { debugf("bsp_load: out of memory\n"); return false; }
    if ((uintptr_t)buf & 7) debugf("bsp_load: WARNING buffer %p not 8-byte aligned\n", buf);
    debugf("bsp_load: %s %d KB in %d ms\n", mapname, size / 1024, (int)(get_ticks_ms() - t0));
    if (memcmp(buf, "HL64", 4)) { free(buf); return false; }
#ifdef HOST_SWAP_BE
    host_swap_hlb(buf);   // PC test harness: convert the big-endian N64 file in place
#endif
    uint32_t *hdr = (uint32_t *)buf;
    world.base = buf;
    bsp_load_serial++;
    world.vertscale = hdr[2];
    uint32_t *tab = hdr + 4;
#define LUMP(i, ptr, cnt) do { ptr = (void *)(buf + tab[(i) * 2]); cnt = tab[(i) * 2 + 1]; } while (0)
    int dummy;
    LUMP(0, world.planes, world.numplanes);
    LUMP(1, world.clipnodes, world.numclipnodes);
    LUMP(2, world.nodes, world.numnodes);
    LUMP(3, world.leafs, world.numleafs);
    LUMP(4, world.marks, world.nummarks);
    LUMP(5, world.vis, world.visbytes);
    LUMP(6, world.bmodels, world.nummodels);
    LUMP(7, world.faces, world.numfaces);
    LUMP(8, world.facetex, dummy);
    LUMP(9, world.chunks, world.numchunks);
    LUMP(10, world.verts, world.numverts);
    LUMP(11, world.tris, world.numtris);
    LUMP(12, world.textures, world.numtextures);
    LUMP(13, world.texdata, dummy);
    LUMP(14, world.entities, dummy);
    world.lightstyles = NULL; world.numlightstyles = 0;
    if (hdr[3] > 15) LUMP(15, world.lightstyles, world.numlightstyles);
    (void)dummy;
    world.texblock = NULL;
    if (hdr[1] >= 2) {
        // version 2: texels live in the shared texture pack, load the ones this map uses
        int total = 0;
        for (int i = 0; i < world.numtextures; i++) {
            btexture_t *t = &world.textures[i];
            total += ((t->w * t->h + 15) & ~15) + 512;
        }
        world.texblock = memalign(16, total);
        FILE *pk = fopen("rom:/textures.pak", "rb");
        if (!world.texblock || !pk) { debugf("texture pack missing\n"); if (pk) fclose(pk); return false; }
        int at = 0;
        for (int i = 0; i < world.numtextures; i++) {
            btexture_t *t = &world.textures[i];
            int n = t->w * t->h;
            fseek(pk, t->dataofs, SEEK_SET);
            fread(world.texblock + at, 1, n, pk);
            t->dataofs = at;
            at += (n + 15) & ~15;
            fseek(pk, t->palofs, SEEK_SET);
            fread(world.texblock + at, 1, 512, pk);
            t->palofs = at;
            at += 512;
        }
        fclose(pk);
        world.texdata = world.texblock;
        data_cache_hit_writeback(world.texblock, total);
    }
    // the RSP and RDP read vertices and textures with DMA
    data_cache_hit_writeback(buf, size);

    world.models = calloc(world.nummodels, sizeof(model_t));
    for (int i = 0; i < world.nummodels; i++) {
        model_t *m = &world.models[i];
        m->bm = &world.bmodels[i];
        m->index = i;
        for (int h = 0; h < 4; h++) {
            hull_t *hl = &m->hulls[h];
            hl->clipnodes = world.clipnodes;
            hl->planes = world.planes;
            hl->firstclipnode = m->bm->headnode[h];
            hl->lastclipnode = world.numclipnodes - 1;
            memcpy(hl->clip_mins, hull_sizes[h][0], 12);
            memcpy(hl->clip_maxs, hull_sizes[h][1], 12);
        }
    }
    strncpy(world.name, mapname, sizeof(world.name) - 1);
    debugf("loaded %s: %d faces %d verts %d tris %d textures (%d KB)\n", mapname, world.numfaces,
           world.numverts, world.numtris, world.numtextures, size / 1024);
    return true;
}

static inline float plane_diff(const bplane_t *pl, const float *p)
{
    if (pl->type < 3) return p[pl->type] - pl->dist;
    return pl->normal[0] * p[0] + pl->normal[1] * p[1] + pl->normal[2] * p[2] - pl->dist;
}

int bsp_leaf_for_point(const float *p)
{
    int n = world.bmodels[0].headnode[0];
    while (n >= 0) {
        bnode_t *nd = &world.nodes[n];
        n = plane_diff(&world.planes[nd->planenum], p) >= 0 ? nd->children[0] : nd->children[1];
    }
    return -1 - n;
}

static uint8_t pvsbuf[(8192 + 7) / 8];
const uint8_t *bsp_leaf_pvs(int leaf)
{
    int rowbytes = (world.bmodels[0].visleafs + 7) >> 3;
    if (leaf <= 0 || world.leafs[leaf].visofs < 0 || !world.visbytes) {
        memset(pvsbuf, 0xff, rowbytes);
        return pvsbuf;
    }
    const uint8_t *in = world.vis + world.leafs[leaf].visofs;
    uint8_t *out = pvsbuf;
    while (out - pvsbuf < rowbytes) {
        if (*in) { *out++ = *in++; continue; }
        int c = in[1];
        in += 2;
        while (c-- && out - pvsbuf < rowbytes) *out++ = 0;
    }
    return pvsbuf;
}

int bsp_hull_point_contents(hull_t *hull, int num, const float *p)
{
    while (num >= 0) {
        bclipnode_t *node = &hull->clipnodes[num];
        num = plane_diff(&hull->planes[node->planenum], p) < 0 ? node->children[1] : node->children[0];
    }
    return num;
}

int bsp_point_contents(const float *p)
{
    return bsp_hull_point_contents(&world.models[0].hulls[0], world.models[0].hulls[0].firstclipnode, p);
}

// ------------------------------------------------------------------ hull trace
#define DIST_EPSILON (0.03125f)

static bool recursive_hull_check(hull_t *hull, int num, float p1f, float p2f,
                                 const float *p1, const float *p2, btrace_t *tr)
{
    if (num < 0) {
        if (num != CONTENTS_SOLID) {
            tr->allsolid = false;
            if (num == CONTENTS_EMPTY) tr->inopen = true;
            else tr->inwater = true;
        } else {
            tr->startsolid = true;
        }
        return true;
    }
    bclipnode_t *node = &hull->clipnodes[num];
    bplane_t *plane = &hull->planes[node->planenum];
    float t1 = plane_diff(plane, p1);
    float t2 = plane_diff(plane, p2);
    if (t1 >= 0 && t2 >= 0) return recursive_hull_check(hull, node->children[0], p1f, p2f, p1, p2, tr);
    if (t1 < 0 && t2 < 0) return recursive_hull_check(hull, node->children[1], p1f, p2f, p1, p2, tr);

    float frac = (t1 < 0) ? (t1 + DIST_EPSILON) / (t1 - t2) : (t1 - DIST_EPSILON) / (t1 - t2);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    float midf = p1f + (p2f - p1f) * frac;
    float mid[3];
    for (int i = 0; i < 3; i++) mid[i] = p1[i] + frac * (p2[i] - p1[i]);
    int side = (t1 < 0);

    if (!recursive_hull_check(hull, node->children[side], p1f, midf, p1, mid, tr)) return false;

    if (bsp_hull_point_contents(hull, node->children[side ^ 1], mid) != CONTENTS_SOLID)
        return recursive_hull_check(hull, node->children[side ^ 1], midf, p2f, mid, p2, tr);

    if (tr->allsolid) return false;

    if (!side) {
        memcpy(tr->normal, plane->normal, 12);
        tr->dist = plane->dist;
    } else {
        for (int i = 0; i < 3; i++) tr->normal[i] = -plane->normal[i];
        tr->dist = -plane->dist;
    }
    while (bsp_hull_point_contents(hull, hull->firstclipnode, mid) == CONTENTS_SOLID) {
        frac -= 0.1f;
        if (frac < 0) {
            tr->fraction = midf;
            memcpy(tr->endpos, mid, 12);
            return false;
        }
        midf = p1f + (p2f - p1f) * frac;
        for (int i = 0; i < 3; i++) mid[i] = p1[i] + frac * (p2[i] - p1[i]);
    }
    tr->fraction = midf;
    memcpy(tr->endpos, mid, 12);
    return false;
}

void bsp_trace_hull(hull_t *hull, const float *start, const float *end, btrace_t *tr)
{
    memset(tr, 0, sizeof(*tr));
    tr->fraction = 1.0f;
    tr->allsolid = true;
    memcpy(tr->endpos, end, 12);
    recursive_hull_check(hull, hull->firstclipnode, 0, 1, start, end, tr);
}

// ------------------------------------------------------------------ texture under a line
// Used by pm_shared for footstep materials (PM_TraceTexture).
static int trace_tex_r(int node, const float *p1, const float *p2, float *hit)
{
    if (node < 0) return -1;
    bnode_t *nd = &world.nodes[node];
    bplane_t *pl = &world.planes[nd->planenum];
    float d1 = plane_diff(pl, p1), d2 = plane_diff(pl, p2);
    int side = d1 < 0;
    if ((d2 < 0) == side) return trace_tex_r(nd->children[side], p1, p2, hit);
    float frac = d1 / (d1 - d2);
    float mid[3];
    for (int i = 0; i < 3; i++) mid[i] = p1[i] + (p2[i] - p1[i]) * frac;
    int r = trace_tex_r(nd->children[side], p1, mid, hit);
    if (r >= 0) return r;
    for (int i = 0; i < nd->numfaces; i++) {
        int fi = nd->firstface + i;
        bfacetex_t *ft = &world.facetex[fi];
        float s = mid[0] * ft->s[0] + mid[1] * ft->s[1] + mid[2] * ft->s[2] + ft->s[3];
        float t = mid[0] * ft->t[0] + mid[1] * ft->t[1] + mid[2] * ft->t[2] + ft->t[3];
        if (s < ft->texmins[0] || t < ft->texmins[1]) continue;
        if (s - ft->texmins[0] > ft->extents[0] || t - ft->texmins[1] > ft->extents[1]) continue;
        memcpy(hit, mid, 12);
        return ft->miptex;
    }
    return trace_tex_r(nd->children[side ^ 1], mid, p2, hit);
}

// face hit by a line (world only), or -1
static int trace_face_r(int node, const float *p1, const float *p2)
{
    if (node < 0) return -1;
    bnode_t *nd = &world.nodes[node];
    bplane_t *pl = &world.planes[nd->planenum];
    float d1 = plane_diff(pl, p1), d2 = plane_diff(pl, p2);
    int side = d1 < 0;
    if ((d2 < 0) == side) return trace_face_r(nd->children[side], p1, p2);
    float frac = d1 / (d1 - d2);
    float mid[3];
    for (int i = 0; i < 3; i++) mid[i] = p1[i] + (p2[i] - p1[i]) * frac;
    int r = trace_face_r(nd->children[side], p1, mid);
    if (r >= 0) return r;
    if (bsp_point_contents(mid) == CONTENTS_SOLID && 0) return -1;
    for (int i = 0; i < nd->numfaces; i++) {
        int fi = nd->firstface + i;
        bfacetex_t *ft = &world.facetex[fi];
        float s = mid[0] * ft->s[0] + mid[1] * ft->s[1] + mid[2] * ft->s[2] + ft->s[3];
        float t = mid[0] * ft->t[0] + mid[1] * ft->t[1] + mid[2] * ft->t[2] + ft->t[3];
        if (s < ft->texmins[0] || t < ft->texmins[1]) continue;
        if (s - ft->texmins[0] > ft->extents[0] || t - ft->texmins[1] > ft->extents[1]) continue;
        return fi;
    }
    return trace_face_r(nd->children[side ^ 1], mid, p2);
}

// Approximate light at a point: the baked vertex colour of the surface below it
// (the PC engine samples the lightmap the same way for studio models).
void bsp_light_at(const float *p, uint8_t *rgb)
{
    float a[3] = {p[0], p[1], p[2] + 8}, b[3] = {p[0], p[1], p[2] - 512};
    int fi = trace_face_r(world.bmodels[0].headnode[0], a, b);
    rgb[0] = rgb[1] = rgb[2] = 160;
    if (fi < 0 || !world.faces[fi].numchunks) return;
    bchunk_t *ch = &world.chunks[world.faces[fi].firstchunk];
    uint32_t sum[3] = {0, 0, 0};
    int n = 0;
    for (int v = 0; v < ch->numverts; v++) {
        T3DVertPacked *pk = &world.verts[(ch->firstvert + v) >> 1];
        uint32_t c = ((ch->firstvert + v) & 1) ? pk->rgbaB : pk->rgbaA;
        sum[0] += c >> 24; sum[1] += (c >> 16) & 0xff; sum[2] += (c >> 8) & 0xff;
        n++;
    }
    if (!n) return;
    for (int i = 0; i < 3; i++) {
        int v = sum[i] / n + 24;      // studio models are never fully black on PC (ambient minimum)
        rgb[i] = v > 255 ? 255 : v;
    }
}

// R_DecalNode: down the tree; where a node's plane passes within 4 units of the decal, every
// surface on it whose texture-space span the decal overlaps gets a piece of it
typedef struct { const float *pos; float w, h, size; bdecal_t *out; int n, max; } dwalk_t;
static void decal_surface(dwalk_t *dw, int fi)
{
    bface_t *f = &world.faces[fi];
    if (f->flags & (FF_SKY | FF_NODRAW | FF_WATER) || !f->numchunks) return;
    bfacetex_t *ft = &world.facetex[fi];
    const float *S = ft->s, *T = ft->t;
    float ls = sqrtf(S[0] * S[0] + S[1] * S[1] + S[2] * S[2]), lt = sqrtf(T[0] * T[0] + T[1] * T[1] + T[2] * T[2]);
    if (ls < 1e-5f || lt < 1e-5f) return;
    // the surface's exact span in texels
    float smin = 1e9f, smax = -1e9f, tmin = 1e9f, tmax = -1e9f, inv = 1.0f / world.vertscale, best = 1e18f;
    uint32_t col = 0xffffffff;
    for (int c = 0; c < f->numchunks; c++) {
        bchunk_t *ch = &world.chunks[f->firstchunk + c];
        for (int v = 0; v < ch->numverts; v++) {
            int vi = ch->firstvert + v;
            const int16_t *q = (vi & 1) ? world.verts[vi >> 1].posB : world.verts[vi >> 1].posA;
            float p[3] = {q[0] * inv, q[1] * inv, q[2] * inv};
            float a = p[0] * S[0] + p[1] * S[1] + p[2] * S[2] + S[3], b = p[0] * T[0] + p[1] * T[1] + p[2] * T[2] + T[3];
            smin = fminf(smin, a); smax = fmaxf(smax, a);
            tmin = fminf(tmin, b); tmax = fmaxf(tmax, b);
            // the light there: the baked colour of the surface's nearest vertex
            float dd = 0;
            for (int k = 0; k < 3; k++) dd += (p[k] - dw->pos[k]) * (p[k] - dw->pos[k]);
            if (dd < best) { best = dd; col = (vi & 1) ? world.verts[vi >> 1].rgbaB : world.verts[vi >> 1].rgbaA; }
        }
    }
    const float *P = dw->pos;
    float ps = P[0] * S[0] + P[1] * S[1] + P[2] * S[2] + S[3], pt = P[0] * T[0] + P[1] * T[1] + P[2] * T[2] + T[3];
    float hw = dw->w * ls * 0.5f, hh = dw->h * lt * 0.5f;   // the decal's half size in this surface's texels
    float s0 = fmaxf(ps - hw, smin), s1 = fminf(ps + hw, smax), t0 = fmaxf(pt - hh, tmin), t1 = fminf(pt + hh, tmax);
    if (s1 - s0 < 0.5f || t1 - t0 < 0.5f) return;
    if (dw->n >= dw->max) return;
    // (s, t) -> the point on the plane: rows S, T, N; solved by cross products
    bplane_t *pl = &world.planes[f->planenum];
    const float *N = pl->normal;
    float tn[3] = {T[1] * N[2] - T[2] * N[1], T[2] * N[0] - T[0] * N[2], T[0] * N[1] - T[1] * N[0]};
    float ns[3] = {N[1] * S[2] - N[2] * S[1], N[2] * S[0] - N[0] * S[2], N[0] * S[1] - N[1] * S[0]};
    float st[3] = {S[1] * T[2] - S[2] * T[1], S[2] * T[0] - S[0] * T[2], S[0] * T[1] - S[1] * T[0]};
    float det = S[0] * tn[0] + S[1] * tn[1] + S[2] * tn[2];
    if (fabsf(det) < 1e-6f) return;
    bdecal_t *d = &dw->out[dw->n++];
    d->face = fi;
    d->rgb[0] = col >> 24; d->rgb[1] = col >> 16; d->rgb[2] = col >> 8;
    float sgn = f->side ? -1.0f : 1.0f;
    for (int k = 0; k < 3; k++) d->n[k] = N[k] * sgn;
    for (int q = 0; q < 4; q++) {
        float cs = (q & 1) ? s1 : s0, ct = (q & 2) ? t1 : t0;
        float r0 = cs - S[3], r1 = ct - T[3], r2 = pl->dist;
        for (int k = 0; k < 3; k++) d->c[q][k] = (r0 * tn[k] + r1 * ns[k] + r2 * st[k]) / det;
        d->uv[q][0] = (cs - (ps - hw)) / (2 * hw);
        d->uv[q][1] = (ct - (pt - hh)) / (2 * hh);
    }
}
static void decal_node(dwalk_t *dw, int nn)
{
    while (nn >= 0) {
        bnode_t *node = &world.nodes[nn];
        bplane_t *pl = &world.planes[node->planenum];
        float dist = dw->pos[0] * pl->normal[0] + dw->pos[1] * pl->normal[1] + dw->pos[2] * pl->normal[2] - pl->dist;
        if (dist > dw->size) { nn = node->children[0]; continue; }
        if (dist < -dw->size) { nn = node->children[1]; continue; }
        if (dist < 4 && dist > -4)
            for (int i = 0; i < node->numfaces; i++) decal_surface(dw, node->firstface + i);
        decal_node(dw, node->children[0]);
        nn = node->children[1];
    }
}
int bsp_decal_faces(int modelindex, const float *pos, float w, float h, bdecal_t *out, int max)
{
    if (modelindex < 0 || modelindex >= world.nummodels) return 0;
    dwalk_t dw = {pos, w, h, fmaxf(w, h) * 0.5f, out, 0, max};
    decal_node(&dw, world.bmodels[modelindex].headnode[0]);
    return dw.n;
}

// the face a line from start to end hits first (in model `modelindex`, placed at `offset`), -1 none
int bsp_trace_face(int modelindex, const float *offset, const float *start, const float *end)
{
    float s[3], e[3];
    for (int i = 0; i < 3; i++) { s[i] = start[i] - offset[i]; e[i] = end[i] - offset[i]; }
    return trace_face_r(world.bmodels[modelindex].headnode[0], s, e);
}

const char *bsp_trace_texture(int modelindex, const float *offset, const float *start, const float *end)
{
    float s[3], e[3], hit[3];
    for (int i = 0; i < 3; i++) { s[i] = start[i] - offset[i]; e[i] = end[i] - offset[i]; }
    int t = trace_tex_r(world.bmodels[modelindex].headnode[0], s, e, hit);
    if (t < 0 || t >= world.numtextures) return NULL;
    return world.textures[t].name;
}
