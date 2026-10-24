// HL64 - world and brush-entity renderer (tiny3d on the RSP, CI8 textures on the RDP).
#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include "bsp.h"
#include "game.h"
#include "render.h"
#include "studio.h"
#include "npc.h"

#define FB_COUNT 3
#define MAX_ENT_MATS 128

static T3DViewport viewport;
static T3DMat4FP *mat_pool;          // FB_COUNT * (1 + MAX_ENT_MATS), uncached
static int frame_index;

static int framecount = 1;
#define ENT_MAX_LEAFS 40
typedef struct { float org[3]; int16_t n; uint16_t leaf[ENT_MAX_LEAFS]; } ent_leafs_t;
static ent_leafs_t *ent_leafs;
static int ent_leafs_count;

static int *face_visframe;
static uint8_t *leaf_inpvs;   // 1 = leaf is in the PVS of the camera leaf
static int32_t *tex_chain;           // head face per texture
static int32_t *face_next;
static int cached_leaf = -2;
static int32_t *vis_faces;
static int16_t (*face_box)[6];   // per-face world-space AABB (mins, maxs) for frustum culling
static int num_vis_faces;
static int numvisleafs;
static uint16_t *visleaf_list;

render_stats_t rstats;
#define FOG_START 900.0f
#define FOG_END 2600.0f
static bool fog_on;
color_t sky_color = {0x40, 0x48, 0x58, 0xff};

static uint8_t *sky_data;              // 'SKY1' + 6 x (palette + 4 tiles)
static T3DVertPacked *sky_verts;       // 6 faces x 4 tiles x 4 verts
static T3DMat4FP *sky_mats;            // one per framebuffer
static rspq_block_t *sky_block;        // pre-recorded uploads + draws for the 24 sky tiles

void render_init(void)
{
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, FB_COUNT, GAMMA_NONE, FILTERS_RESAMPLE_ANTIALIAS_DEDITHER);
    rdpq_init();
    t3d_init((T3DInitParams){});
    viewport = t3d_viewport_create_buffered(FB_COUNT);
    mat_pool = malloc_uncached(sizeof(T3DMat4FP) * FB_COUNT * (1 + MAX_ENT_MATS));
    // long-lived buffers are allocated up front, so they don't split the heap between maps
    sky_data = (uint8_t *)memalign(16, 16 + 6 * (512 + 4096)) + 12;   /* header at +12 so the texels after it are 16-byte aligned (the RDP hangs on misaligned loads) */
    sky_verts = malloc_uncached(sizeof(T3DVertPacked) * 48);
    sky_mats = malloc_uncached(sizeof(T3DMat4FP) * FB_COUNT);
    studio_begin_frame(0);
    rdpq_text_register_font(FONT_BUILTIN_DEBUG_MONO, rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO));
}

static void free_texture_blocks(void);
static void record_texture_blocks(void);
static void build_strips(void);
static void record_model_blocks(void);
static int *model_tris;   // triangles per brush model (stats)
static void free_model_blocks(void);

void render_new_map(void)
{
    free_model_blocks();
    free_texture_blocks();
    record_texture_blocks();
    build_strips();
    record_model_blocks();
    free(face_visframe); free(tex_chain); free(face_next); free(visleaf_list);
    face_visframe = calloc(world.numfaces, sizeof(int));
    tex_chain = malloc(world.numtextures * sizeof(int32_t));
    face_next = malloc(world.numfaces * sizeof(int32_t));
    visleaf_list = malloc(world.numleafs * sizeof(uint16_t));
    free(leaf_inpvs);
    leaf_inpvs = calloc(world.numleafs, 1);
    free(vis_faces);
    vis_faces = malloc(world.numfaces * sizeof(int32_t));
    free(ent_leafs);
    ent_leafs_count = num_entities;
    ent_leafs = malloc(sizeof(ent_leafs_t) * ent_leafs_count);
    if (ent_leafs) for (int i = 0; i < ent_leafs_count; i++) ent_leafs[i].n = -2;
    free(face_box);
    face_box = malloc(world.numfaces * sizeof(*face_box));
    float inv = 1.0f / world.vertscale;
    for (int fi = 0; fi < world.numfaces; fi++) {
        bface_t *f = &world.faces[fi];
        float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
        for (int c = 0; c < f->numchunks; c++) {
            bchunk_t *ch = &world.chunks[f->firstchunk + c];
            for (int v = 0; v < ch->numverts; v++) {
                int vi = ch->firstvert + v;
                const int16_t *pos = (vi & 1) ? world.verts[vi >> 1].posB : world.verts[vi >> 1].posA;
                for (int k = 0; k < 3; k++) { float x = pos[k] * inv; if (x < mn[k]) mn[k] = x; if (x > mx[k]) mx[k] = x; }
            }
        }
        for (int k = 0; k < 3; k++) {
            if (mn[k] > mx[k]) { mn[k] = 32000; mx[k] = -32000; }
            face_box[fi][k] = (int16_t)fmaxf(-32767, floorf(mn[k]) - 1);
            face_box[fi][k + 3] = (int16_t)fminf(32767, ceilf(mx[k]) + 1);
        }
    }
    debugf("face bounds: %d faces (%d KB)\n", world.numfaces, (int)(world.numfaces * sizeof(*face_box) / 1024));
    cached_leaf = -2;
    framecount = 1;
}

// ---------------------------------------------------------------- frustum
typedef struct { float n[3]; float d; } fplane_t;
static fplane_t frustum[4];

static void setup_frustum(const float *eye, const float *f, const float *r, const float *u, float fovx, float fovy)
{
    float a = fovx * 0.5f * (M_PI / 180), b = fovy * 0.5f * (M_PI / 180);
    float sa = sinf(a), ca = cosf(a), sb = sinf(b), cb = cosf(b);
    for (int i = 0; i < 3; i++) {
        frustum[0].n[i] = f[i] * sa + r[i] * ca;  // left
        frustum[1].n[i] = f[i] * sa - r[i] * ca;  // right
        frustum[2].n[i] = f[i] * sb + u[i] * cb;  // bottom
        frustum[3].n[i] = f[i] * sb - u[i] * cb;  // top
    }
    for (int p = 0; p < 4; p++)
        frustum[p].d = frustum[p].n[0] * eye[0] + frustum[p].n[1] * eye[1] + frustum[p].n[2] * eye[2];
}

static bool box_culled(const float *mins, const float *maxs)
{
    for (int p = 0; p < 4; p++) {
        const float *n = frustum[p].n;
        float x = n[0] >= 0 ? maxs[0] : mins[0];
        float y = n[1] >= 0 ? maxs[1] : mins[1];
        float z = n[2] >= 0 ? maxs[2] : mins[2];
        if (n[0] * x + n[1] * y + n[2] * z - frustum[p].d < -8.0f) return true;
    }
    return false;
}

// ---------------------------------------------------------------- texture binding
static int bound_tex = -1;
static int bound_alpha = -1;

uint32_t prof_bind_ticks, prof_binds, prof_flush_ticks;
static rspq_block_t **tex_blocks;   // pre-recorded TLUT + texture upload per texture
static int num_tex_blocks;

static void record_texture_blocks(void)
{
    num_tex_blocks = world.numtextures;
    tex_blocks = calloc(num_tex_blocks, sizeof(rspq_block_t *));
    for (int t = 0; t < num_tex_blocks; t++) {
        btexture_t *tx = &world.textures[t];
        if (tx->flags & (FF_NODRAW | FF_SKY)) continue;
        rspq_block_begin();
        rdpq_tex_upload_tlut((uint16_t *)(world.texdata + tx->palofs), 0, 256);
        surface_t surf = surface_make_linear(world.texdata + tx->dataofs, FMT_CI8, tx->w, tx->h);
        rdpq_tex_upload(TILE0, &surf, &(rdpq_texparms_t){
            .s.repeats = REPEAT_INFINITE, .t.repeats = REPEAT_INFINITE,
        });
        tex_blocks[t] = rspq_block_end();
    }
}

static void free_texture_blocks(void)
{
    if (!tex_blocks) return;
    rspq_wait();
    for (int t = 0; t < num_tex_blocks; t++)
        if (tex_blocks[t]) rspq_block_free(tex_blocks[t]);
    free(tex_blocks);
    tex_blocks = NULL;
}

static void bind_texture(int t)
{
    if (t == bound_tex) return;
    uint32_t a = TICKS_READ();
    bound_tex = t;
    btexture_t *tx = &world.textures[t];
    int alpha = (tx->flags & FF_ALPHATEST) ? 1 : 0;
    if (alpha != bound_alpha) {
        rdpq_mode_alphacompare(alpha ? 128 : 0);
        bound_alpha = alpha;
    }
    if (tex_blocks[t]) rspq_block_run(tex_blocks[t]);
    prof_bind_ticks += TICKS_DISTANCE(a, TICKS_READ());
    prof_binds++;
}

// ---------------------------------------------------------------- chunk drawing
// Each chunk (<= 64 vertices) is drawn with one vertex load and one or more tiny3d strip
// commands whose index lists were built at map load; the RSP fetches them by DMA, so the
// CPU issues a handful of commands per chunk instead of one per triangle.
typedef struct { uint32_t first; uint16_t nstrips, pad; } chunkstrips_t;
typedef struct { uint32_t offset; uint16_t count, pad; } strip_t;  // offset in int16 units, 8-byte aligned
static chunkstrips_t *chunk_strips;
static strip_t *strips;
static int16_t *strip_idx;

static void build_strips(void)
{
    free(chunk_strips); free(strips); free(strip_idx);
    chunk_strips = malloc(sizeof(chunkstrips_t) * (world.numchunks + 1));
    // worst case: every triangle restarts (3 indices) plus alignment padding per strip
    int maxidx = world.numtris * 3 + world.numchunks * 8 + 64;
    int16_t *idx = memalign(16, maxidx * sizeof(int16_t));
    strip_t *st = malloc(sizeof(strip_t) * (world.numchunks * 4 + 16));
    int nidx = 0, nst = 0, stcap = world.numchunks * 4 + 16;
    for (int c = 0; c < world.numchunks; c++) {
        bchunk_t *ch = &world.chunks[c];
        chunk_strips[c].first = nst;
        chunk_strips[c].nstrips = 0;
        // indices are DMA'd into DMEM just after the vertex cache: keep each list short enough
        int limit = ((T3D_VERTEX_CACHE_SIZE - ch->numverts) * 36) / 2 - 8;
        if (limit > 240) limit = 240;
        if (limit < 6) limit = 6;
        const uint8_t *tri = world.tris + ch->firsttri * 3;
        int start = -1, a = -1, b = -1;
        for (int i = 0; i < ch->numtris; i++) {
            int t0 = tri[i * 3], t1 = tri[i * 3 + 1], t2 = tri[i * 3 + 2];
            int ext = -1;
            if (start >= 0 && nidx - start + 1 <= limit) {
                // can the strip (…, a, b) continue with this triangle?
                int v[3] = {t0, t1, t2};
                for (int k = 0; k < 3; k++) {
                    int x = v[k], y = v[(k + 1) % 3], z = v[(k + 2) % 3];
                    if ((y == a && z == b) || (y == b && z == a)) { ext = x; break; }
                    (void)x;
                }
            }
            if (ext >= 0) {
                idx[nidx++] = ext;
                a = b; b = ext;
                continue;
            }
            if (start < 0 || nidx - start + 3 > limit) {
                // close the current strip, begin a new one (8-byte aligned)
                if (start >= 0) {
                    st[nst - 1].count = nidx - start;
                    while (nidx & 3) idx[nidx++] = 0;
                }
                if (nst >= stcap) { stcap *= 2; st = realloc(st, sizeof(strip_t) * stcap); }
                start = nidx;
                st[nst].offset = nidx;
                st[nst].count = 0;
                nst++;
                chunk_strips[c].nstrips++;
                idx[nidx++] = t0; idx[nidx++] = t1; idx[nidx++] = t2;
            } else {
                idx[nidx++] = (int16_t)(t0 | 0x8000); idx[nidx++] = t1; idx[nidx++] = t2;
            }
            a = t1; b = t2;
        }
        if (start >= 0) {
            st[nst - 1].count = nidx - start;
            while (nidx & 3) idx[nidx++] = 0;
        }
        start = -1;
    }
    // convert each strip to tiny3d's DMEM-pointer format
    for (int i = 0; i < nst; i++) t3d_indexbuffer_convert(&idx[st[i].offset], st[i].count);
    data_cache_hit_writeback(idx, nidx * sizeof(int16_t));
    strip_idx = idx;
    strips = st;
    debugf("strips: %d chunks -> %d strips, %d indices (%d KB)\n", world.numchunks, nst, nidx, nidx * 2 / 1024);
}

static inline void flush_cache(void) { }

static void draw_face(int fi)
{
    bface_t *f = &world.faces[fi];
    for (int c = 0; c < f->numchunks; c++) {
        int ci = f->firstchunk + c;
        bchunk_t *ch = &world.chunks[ci];
        chunkstrips_t *cs = &chunk_strips[ci];
        if (!cs->nstrips) continue;
#ifdef EXP_NOLOAD
        if (0)
#endif
        t3d_vert_load(&world.verts[ch->firstvert >> 1], 0, ch->numverts);
#ifdef EXP_NOSTRIP
        if (0)
#endif
        for (int k = 0; k < cs->nstrips; k++) {
            strip_t *sp = &strips[cs->first + k];
            if (k == cs->nstrips - 1) t3d_tri_draw_strip_and_sync(&strip_idx[sp->offset], sp->count);
            else t3d_tri_draw_strip(&strip_idx[sp->offset], sp->count);
        }
        rstats.tris += ch->numtris;
    }
}

static inline bool face_back(int fi, const float *eye)
{
    bface_t *f = &world.faces[fi];
    bplane_t *pl = &world.planes[f->planenum];
    float d = pl->normal[0] * eye[0] + pl->normal[1] * eye[1] + pl->normal[2] * eye[2] - pl->dist;
    if (f->side) d = -d;
    return d < 0.01f;
}

// ---------------------------------------------------------------- world
static void mark_visible(const float *eye)
{
    int leaf = bsp_leaf_for_point(eye);
    if (leaf != cached_leaf) {
        cached_leaf = leaf;
        const uint8_t *pvs = bsp_leaf_pvs(leaf);
        int nvis = world.bmodels[0].visleafs;
        numvisleafs = 0;
        memset(leaf_inpvs, 0, world.numleafs);
        for (int i = 0; i < nvis; i++)
            if (pvs[i >> 3] & (1 << (i & 7))) { visleaf_list[numvisleafs++] = i + 1; leaf_inpvs[i + 1] = 1; }
    }
    rstats.leaf = leaf;
    num_vis_faces = 0;
    for (int k = 0; k < numvisleafs; k++) {
        bleaf_t *lf = &world.leafs[visleaf_list[k]];
        float mn[3] = {lf->mins[0], lf->mins[1], lf->mins[2]}, mx[3] = {lf->maxs[0], lf->maxs[1], lf->maxs[2]};
        if (box_culled(mn, mx)) continue;
        uint16_t *m = &world.marks[lf->firstmark];
        for (int i = 0; i < lf->nummark; i++) {
            int fi = m[i];
            if (face_visframe[fi] != framecount) {
                face_visframe[fi] = framecount;
                vis_faces[num_vis_faces++] = fi;
            }
        }
    }
}

static bool sky_visible;
#ifdef CULLDBG
static int cull_n, cull_frames;
#define CULLDBG_INC cull_n++
#define CULLDBG_PRINT if (++cull_frames % 30 == 0) debugf("cull: drawn %d faces, face-bounds culled %d more\n", nf, cull_n); cull_n = 0
#else
#define CULLDBG_INC
#define CULLDBG_PRINT
#endif

static void collect_world(const float *eye)
{
    sky_visible = false;
    for (int t = 0; t < world.numtextures; t++) tex_chain[t] = -1;
    bmodel_t *wm = &world.bmodels[0];
    int nf = 0;
    int wlast = wm->firstface + wm->numfaces;
    for (int k = 0; k < num_vis_faces; k++) {
        int fi = vis_faces[k];
        if (fi < wm->firstface || fi >= wlast) continue;
        bface_t *f = &world.faces[fi];
        if (f->flags & FF_SKY) { if (!sky_visible && !face_back(fi, eye)) sky_visible = true; continue; }
        if (f->flags & FF_NODRAW || !f->numchunks) continue;
        if (face_back(fi, eye)) continue;
        {   // drop faces whose own bounds are outside the view (big leaves straddle the frustum)
            const int16_t *b = face_box[fi];
            float mn[3] = {b[0], b[1], b[2]}, mx[3] = {b[3], b[4], b[5]};
            if (box_culled(mn, mx)) { CULLDBG_INC; continue; }
        }
        face_next[fi] = tex_chain[f->texture];
        tex_chain[f->texture] = fi;
        nf++;
    }
    rstats.faces = nf;
    CULLDBG_PRINT;
}

static void draw_world(void)
{
    for (int t = 0; t < world.numtextures; t++) {
        if (tex_chain[t] < 0) continue;
        flush_cache();
        bind_texture(t);
        for (int fi = tex_chain[t]; fi >= 0; fi = face_next[fi]) draw_face(fi);
    }
    flush_cache();
}

// ---------------------------------------------------------------- brush entities
static rspq_block_t **model_blocks;
static int num_model_blocks;

// Brush entity models never change, so record their draw commands once per map
// (texture binds + vertex loads + strips); drawing an entity is then one block call.
static void record_model_blocks(void)
{
    num_model_blocks = world.nummodels;
    model_blocks = calloc(num_model_blocks, sizeof(rspq_block_t *));
    for (int m = 1; m < world.nummodels; m++) {
        bmodel_t *bm = &world.bmodels[m];
        if (bm->numfaces <= 0) continue;
        rspq_block_begin();
        int cur_alpha = -1;
        for (int t = 0; t < world.numtextures; t++) {
            bool any = false;
            for (int fi = bm->firstface; fi < bm->firstface + bm->numfaces; fi++) {
                bface_t *f = &world.faces[fi];
                if (f->texture != t || (f->flags & (FF_SKY | FF_NODRAW)) || !f->numchunks) continue;
                if (!any) {
                    int alpha = (world.textures[t].flags & FF_ALPHATEST) ? 1 : 0;
                    if (alpha != cur_alpha) { rdpq_mode_alphacompare(alpha ? 128 : 0); cur_alpha = alpha; }
                    if (tex_blocks[t]) rspq_block_run(tex_blocks[t]);
                    any = true;
                }
                draw_face(fi);
            }
        }
        model_blocks[m] = rspq_block_end();
    }
    free(model_tris);
    model_tris = calloc(world.nummodels, sizeof(int));
    for (int m = 1; model_tris && m < world.nummodels; m++) {
        bmodel_t *bmm = &world.bmodels[m];
        for (int fi = bmm->firstface; fi < bmm->firstface + bmm->numfaces; fi++)
            for (int c = 0; c < world.faces[fi].numchunks; c++)
                model_tris[m] += world.chunks[world.faces[fi].firstchunk + c].numtris;
    }
}

static void free_model_blocks(void)
{
    if (!model_blocks) return;
    rspq_wait();
    for (int m = 0; m < num_model_blocks; m++)
        if (model_blocks[m]) rspq_block_free(model_blocks[m]);
    free(model_blocks);
    model_blocks = NULL;
}

static void build_entity_matrix(T3DMat4FP *out, const float *origin, const float *angles, float s)
{
    float sy = sinf(angles[1] * M_PI / 180), cy = cosf(angles[1] * M_PI / 180);
    float sp = sinf(angles[0] * M_PI / 180), cp = cosf(angles[0] * M_PI / 180);
    float sr = sinf(angles[2] * M_PI / 180), cr = cosf(angles[2] * M_PI / 180);
    // GoldSrc AngleMatrix: columns are forward, left, up
    float f[3] = {cp * cy, cp * sy, -sp};
    float l[3] = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp};
    float u[3] = {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
    T3DMat4 m;
    for (int i = 0; i < 3; i++) {
        m.m[0][i] = f[i] * s;
        m.m[1][i] = l[i] * s;
        m.m[2][i] = u[i] * s;
        m.m[3][i] = origin[i];
    }
    m.m[0][3] = m.m[1][3] = m.m[2][3] = 0;
    m.m[3][3] = 1;
    t3d_mat4_to_fixed(out, &m);
}

#ifndef ENT_CULL
#define ENT_CULL T3D_FLAG_CULL_FRONT
#endif
// brush entity PVS test (the PC client does it for every entity): the leafs an entity's box
// touches (SV_FindTouchedLeafs) are gathered when it moves, and each frame one of them has to be in
// the camera's PVS
static void box_leafs_r(int node, const float *mn, const float *mx, ent_leafs_t *el)
{
    while (node >= 0) {
        if (el->n < 0) return;
        bnode_t *n = &world.nodes[node];
        bplane_t *pl = &world.planes[n->planenum];
        float dmin, dmax;
        if (pl->type < 3) { dmin = mn[pl->type] - pl->dist; dmax = mx[pl->type] - pl->dist; }
        else {
            dmin = dmax = -pl->dist;
            for (int k = 0; k < 3; k++) {
                float a = pl->normal[k] * mn[k], b = pl->normal[k] * mx[k];
                dmin += fminf(a, b); dmax += fmaxf(a, b);
            }
        }
        if (dmin >= 0) node = n->children[0];
        else if (dmax < 0) node = n->children[1];
        else { box_leafs_r(n->children[0], mn, mx, el); node = n->children[1]; }
    }
    int leaf = -node - 1;
    if (leaf <= 0 || el->n < 0) return;
    if (el->n >= ENT_MAX_LEAFS) { el->n = -1; return; }   // too big: always drawn (frustum still applies)
    el->leaf[el->n++] = leaf;
}
static bool ent_in_pvs(int i, const entity_t *e, const float *mn, const float *mx)
{
#ifdef NO_ENT_PVS
    return true;
#endif
    if (numvisleafs <= 0 || !ent_leafs || i >= ent_leafs_count) return true;
    ent_leafs_t *el = &ent_leafs[i];
    if (el->n == -2 || el->org[0] != e->origin[0] || el->org[1] != e->origin[1] || el->org[2] != e->origin[2]) {
        memcpy(el->org, e->origin, 12);
        el->n = 0;
        box_leafs_r(world.bmodels[0].headnode[0], mn, mx, el);
    }
    if (el->n < 0) return true;
    for (int k = 0; k < el->n; k++) if (leaf_inpvs[el->leaf[k]]) return true;
    return false;
}
static void draw_entities(const float *eye, T3DMat4FP *mats)
{
    int used = 0;
    t3d_state_set_drawflags(T3D_FLAG_DEPTH | T3D_FLAG_TEXTURED | T3D_FLAG_SHADED | ENT_CULL);
    for (int i = 1; i < num_entities && used < MAX_ENT_MATS; i++) {
        entity_t *e = &entities[i];
        if (!e->visible || e->removed || e->model <= 0) continue;
        bmodel_t *bm = &world.bmodels[e->model];
        bool rotated = e->angles[0] != 0 || e->angles[1] != 0 || e->angles[2] != 0;
        float mn[3], mx[3];
        for (int k = 0; k < 3; k++) {
            if (rotated) {
                float r = fmaxf(fabsf(bm->mins[k]), fabsf(bm->maxs[k]));
                float rr = fmaxf(fmaxf(fabsf(bm->mins[0]), fabsf(bm->maxs[0])), fmaxf(fabsf(bm->mins[1]), fabsf(bm->maxs[1])));
                r = fmaxf(r, fmaxf(rr, fmaxf(fabsf(bm->mins[2]), fabsf(bm->maxs[2]))));
                mn[k] = e->origin[k] - r; mx[k] = e->origin[k] + r;
            } else {
                mn[k] = e->origin[k] + bm->mins[k]; mx[k] = e->origin[k] + bm->maxs[k];
            }
        }
        if (box_culled(mn, mx)) continue;
        if (!ent_in_pvs(i, e, mn, mx)) continue;
        T3DMat4FP *mat = &mats[used++];
        build_entity_matrix(mat, e->origin, e->angles, 1.0f / world.vertscale);
        flush_cache();
        t3d_matrix_push(mat);
        bool trans = (e->rendermode == 2 || e->rendermode == 5) && e->renderamt < 255;
        if (trans) {
            rdpq_sync_pipe();
            rdpq_mode_begin();
            rdpq_mode_blender(e->rendermode == 5 ? RDPQ_BLENDER_ADDITIVE : RDPQ_BLENDER_MULTIPLY);
            rdpq_mode_combiner(RDPQ_COMBINER2((TEX0, 0, SHADE, 0), (0, 0, 0, PRIM), (0, 0, 0, COMBINED), (0, 0, 0, COMBINED)));
            rdpq_mode_zbuf(true, false);
            rdpq_mode_end();
            rdpq_set_prim_color(RGBA32(255, 255, 255, e->renderamt));
        }
        // back faces are culled by the RSP inside the recorded block (no per-face CPU test)
        if (model_blocks[e->model]) {
            rspq_block_run(model_blocks[e->model]);
            if (model_tris) rstats.tris += model_tris[e->model];
        }
        bound_tex = -1;
        bound_alpha = -1;
        if (trans) {
            rdpq_sync_pipe();
            rdpq_mode_begin();
            rdpq_mode_blender(0);
            rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
            rdpq_mode_zbuf(true, true);
            rdpq_mode_end();
        }
        t3d_matrix_pop(1);
    }
    t3d_state_set_drawflags(T3D_FLAG_DEPTH | T3D_FLAG_TEXTURED | T3D_FLAG_SHADED);
    rstats.ents = used;
}


// ---------------------------------------------------------------- skybox (gfx/env, Quake 2 / GoldSrc layout)
#define SKY_SIZE 1024
static bool sky_ok;

// st_to_vec from Quake 2's gl_warp.c (GoldSrc uses the same convention)
static const int st_to_vec[6][3] = {{3, -1, 2}, {-3, 1, 2}, {1, 3, 2}, {-1, -3, 2}, {-2, -1, 3}, {2, -1, -3}};

static void sky_vec(int face, float s, float t, int16_t *out)
{
    float b[3] = {s * SKY_SIZE, t * SKY_SIZE, SKY_SIZE};
    for (int k = 0; k < 3; k++) {
        int c = st_to_vec[face][k];
        float v = c > 0 ? b[c - 1] : -b[-c - 1];
        out[k] = (int16_t)v;
    }
}

void render_load_sky(const char *name)
{
    sky_ok = false;
    char path[64];
    snprintf(path, sizeof(path), "rom:/sky/%s.sky", name);
    FILE *f = fopen(path, "rb");
    if (!f) { debugf("no sky %s\n", name); return; }
    if (!sky_data) sky_data = (uint8_t *)memalign(16, 16 + 6 * (512 + 4096)) + 12;   /* header at +12 so the texels after it are 16-byte aligned (the RDP hangs on misaligned loads) */
    fread(sky_data, 1, 4 + 6 * (512 + 4096), f);
    fclose(f);
    data_cache_hit_writeback(sky_data, 4 + 6 * (512 + 4096));
    if (!sky_verts) sky_verts = malloc_uncached(sizeof(T3DVertPacked) * 48);
    if (!sky_mats) sky_mats = malloc_uncached(sizeof(T3DMat4FP) * FB_COUNT);
    for (int face = 0; face < 6; face++)
        for (int tile = 0; tile < 4; tile++) {
            int tx = tile & 1, ty = tile >> 1;
            for (int c = 0; c < 4; c++) {
                int cx = (c == 1 || c == 2), cy = (c >= 2);
                float u = (tx * 32 + cx * 32) / 64.0f, v = (ty * 32 + cy * 32) / 64.0f;
                // texture coords (u,v) -> Q2 (s,t): s = 2u-1, t = 1-2v
                int16_t pos[3];
                sky_vec(face, 2 * u - 1, 1 - 2 * v, pos);
                int vi = (face * 4 + tile) * 4 + c;
                T3DVertPacked *p = &sky_verts[vi >> 1];
                int16_t st[2] = {(int16_t)(cx * 32 * 32), (int16_t)(cy * 32 * 32)};
                if (vi & 1) { memcpy(p->posB, pos, 6); p->normB = 0; p->rgbaB = 0xffffffff; memcpy(p->stB, st, 4); }
                else { memcpy(p->posA, pos, 6); p->normA = 0; p->rgbaA = 0xffffffff; memcpy(p->stA, st, 4); }
            }
        }
    if (sky_block) { rspq_wait(); rspq_block_free(sky_block); sky_block = NULL; }
    rspq_block_begin();
    for (int face = 0; face < 6; face++) {
        uint8_t *fd = sky_data + 4 + face * (512 + 4096);
        rdpq_tex_upload_tlut((uint16_t *)fd, 0, 256);
        for (int tile = 0; tile < 4; tile++) {
            surface_t surf = surface_make_linear(fd + 512 + tile * 1024, FMT_CI8, 32, 32);
            rdpq_tex_upload(TILE0, &surf, NULL);
            t3d_vert_load(&sky_verts[(face * 4 + tile) * 2], 0, 4);
            t3d_tri_draw(0, 1, 2);
            t3d_tri_draw(0, 2, 3);
            t3d_tri_sync();
        }
    }
    sky_block = rspq_block_end();
    sky_ok = true;
}

static void draw_sky(const float *eye)
{
    if (!sky_ok) return;
    T3DMat4FP *m = &sky_mats[frame_index];
    t3d_mat4fp_from_srt_euler(m, (float[3]){1, 1, 1}, (float[3]){0, 0, 0}, (float[3]){eye[0], eye[1], eye[2]});
    rdpq_sync_pipe();
    rdpq_mode_begin();
    rdpq_mode_zbuf(false, false);
    rdpq_mode_alphacompare(0);
    rdpq_mode_end();
    t3d_matrix_push(m);
    rspq_block_run(sky_block);
    t3d_matrix_pop(1);
    rdpq_sync_pipe();
    rdpq_mode_begin();
    rdpq_mode_zbuf(true, true);
    rdpq_mode_end();
    bound_tex = -1;
    bound_alpha = 0;
}

// ---------------------------------------------------------------- frame
// ---------------------------------------------------------------- characters
#ifndef NPC_CULL
#define NPC_CULL T3D_FLAG_CULL_FRONT
#endif
#define MAX_DRAWN_NPCS 12
static void draw_npcs(const float *eye)
{
    int order[MAX_NPCS];
    float dist[MAX_NPCS];
    int n = 0;
    for (int i = 0; i < num_npcs; i++) {
        npc_t *c = &npcs[i];
        if (!c->model) continue;
        entity_t *ce = &entities[c->ent];
        if (ce->removed || (ce->rendermode != 0 && ce->renderamt == 0)) continue;   // killed / env_render
        float d2 = 0;
        for (int k = 0; k < 3; k++) { float dd = c->origin[k] - eye[k]; d2 += dd * dd; }
        if (d2 > 2500.0f * 2500.0f) continue;
        float mn[3], mx[3];
        for (int k = 0; k < 3; k++) { mn[k] = c->origin[k] + c->mins[k]; mx[k] = c->origin[k] + c->maxs[k]; }
        mn[2] -= 8; mx[2] += 8;
        if (box_culled(mn, mx)) continue;
        float mid[3] = {c->origin[0], c->origin[1], c->origin[2] + (c->mins[2] + c->maxs[2]) * 0.5f};
        int lf = bsp_leaf_for_point(mid);
        if (lf <= 0 || lf >= world.numleafs || !leaf_inpvs[lf]) {
            lf = bsp_leaf_for_point(c->origin);   // origin can sit exactly on the floor plane
            if (lf <= 0 || lf >= world.numleafs || !leaf_inpvs[lf]) continue;
        }
        if (npc_is_item(i)) d2 *= 4;   // characters before pickups when there are too many to draw
        // insertion sort by distance, keep the nearest
        int j = n++;
        while (j > 0 && dist[j - 1] > d2) { order[j] = order[j - 1]; dist[j] = dist[j - 1]; j--; }
        order[j] = i; dist[j] = d2;
    }
    if (n > MAX_DRAWN_NPCS) n = MAX_DRAWN_NPCS;
    rstats.npcs = n;
    if (!n) return;
    t3d_state_set_drawflags(T3D_FLAG_DEPTH | T3D_FLAG_TEXTURED | T3D_FLAG_SHADED | NPC_CULL);
    bound_tex = -1;
    for (int i = 0; i < n; i++) {
        npc_t *c = &npcs[order[i]];
        studio_draw_t d = {.model = c->model, .yaw = c->yaw, .seq = c->seq, .frame = c->frame,
                           .body = c->body, .skin = c->skin, .att_out = c->att, .att_n = 1, .ctl0 = c->ctl0};
        if (c->model->numatt > 0) { extern float game_time(void); c->att_time = game_time(); }
        memcpy(d.origin, c->origin, 12);
        memcpy(d.light, c->light, 3);
        {   // flashlight dynamic light on characters (R_LightPoint adds dlights within their radius)
            extern bool weapons_flashlight_point(float *out);
            float fp[3];
            if (weapons_flashlight_point(fp)) {
                float dd = 0;
                for (int k = 0; k < 3; k++) { float q = c->origin[k] + (k == 2 ? 36 : 0) - fp[k]; dd += q * q; }
                dd = sqrtf(dd);
                if (dd < 140) {
                    int add = (int)(160 * (1 - dd / 140));
                    for (int k = 0; k < 3; k++) { int v = d.light[k] + add; d.light[k] = v > 255 ? 255 : v; }
                }
            }
        }
        // talking: flap the jaw while the voice channel plays
        if (c->model->mouth_bone >= 0 && snd_playing(c->ent, CHAN_VOICE)) {
            float tt = (float)(get_ticks_ms() % 100000) * 0.001f + order[i];
            float v = 0.5f + 0.3f * sinf(tt * 23.0f) + 0.2f * sinf(tt * 37.0f);
            d.mouth = v < 0 ? 0 : v;
        }
        studio_draw(&d);
    }
    t3d_state_set_drawflags(T3D_FLAG_DEPTH | T3D_FLAG_TEXTURED | T3D_FLAG_SHADED);
    bound_tex = -1;
    bound_alpha = -1;
}

void render_frame(const float *eye, const float *angles, void (*hud)(void))
{
    frame_index = (frame_index + 1) % FB_COUNT;
    framecount++;
    rstats.tris = 0;

    float f[3], r[3], u[3];
    {
        float sy = sinf(angles[1] * M_PI / 180), cy = cosf(angles[1] * M_PI / 180);
        float sp = sinf(angles[0] * M_PI / 180), cp = cosf(angles[0] * M_PI / 180);
        float sr = sinf(angles[2] * M_PI / 180), cr = cosf(angles[2] * M_PI / 180);
        f[0] = cp * cy; f[1] = cp * sy; f[2] = -sp;
        r[0] = -sr * sp * cy + cr * sy; r[1] = -sr * sp * sy - cr * cy; r[2] = -sr * cp;
        u[0] = cr * sp * cy + sr * sy; u[1] = cr * sp * sy - sr * cy; u[2] = cr * cp;
    }
    const float fovx = 90.0f;
    const float fovy = 2.0f * atanf(tanf(fovx * 0.5f * M_PI / 180) * 0.75f) * 180 / M_PI;

    T3DMat4FP *mats = &mat_pool[frame_index * (1 + MAX_ENT_MATS)];
    t3d_mat4fp_from_srt_euler(&mats[0], (float[3]){1.0f / world.vertscale, 1.0f / world.vertscale, 1.0f / world.vertscale},
                              (float[3]){0, 0, 0}, (float[3]){0, 0, 0});

    t3d_viewport_set_projection(&viewport, fovy * M_PI / 180, 4.0f, fog_on ? FOG_END + 64 : 4096.0f);
    fog_on = !sky_visible;   // indoors: fog hides distant geometry, far plane culls it
    T3DVec3 e = {{eye[0], eye[1], eye[2]}};
    T3DVec3 tgt = {{eye[0] + f[0], eye[1] + f[1], eye[2] + f[2]}};
    T3DVec3 up = {{u[0], u[1], u[2]}};
    t3d_viewport_look_at(&viewport, &e, &tgt, &up);

    uint32_t t0 = TICKS_READ();
    setup_frustum(eye, f, r, u, fovx + 4, fovy + 4);
    mark_visible(eye);
    uint32_t t1 = TICKS_READ();
    surface_t *fb = display_get();
    uint32_t t2 = TICKS_READ();
    rstats.t_vis += TICKS_DISTANCE(t0, t1);
    rstats.t_wait += TICKS_DISTANCE(t1, t2);

    rdpq_attach(fb, display_get_zbuf());
    t3d_frame_start();
#ifdef EXP_SCISSOR
    rdpq_set_scissor(0, 0, 32, 24);
#endif
    t3d_viewport_attach(&viewport);
    rdpq_set_fog_color(sky_color);
    t3d_fog_set_range(FOG_START, FOG_END);
    t3d_fog_set_enabled(fog_on);
    rdpq_mode_begin();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_antialias(AA_NONE);
    rdpq_mode_fog(fog_on ? RDPQ_FOG_STANDARD : 0);
    rdpq_mode_end();
    bound_tex = -1;
    bound_alpha = -1;

    collect_world(eye);
    if (!sky_visible || !sky_ok) t3d_screen_clear_color(sky_color);
    t3d_screen_clear_depth();
    uint8_t white[4] = {0xff, 0xff, 0xff, 0xff};
    t3d_light_set_ambient(white);
    t3d_light_set_count(0);
    t3d_state_set_drawflags(T3D_FLAG_DEPTH | T3D_FLAG_TEXTURED | T3D_FLAG_SHADED);

    uint32_t t3 = TICKS_READ();
    if (sky_visible) draw_sky(eye);
    uint32_t t4 = TICKS_READ();
#ifndef NO_WORLD
    t3d_matrix_push(&mats[0]);
    draw_world();
    t3d_matrix_pop(1);
#endif
    uint32_t t5 = TICKS_READ();
#ifndef NO_ENTS
    draw_entities(eye, &mats[1]);
#endif
    uint32_t t5a = TICKS_READ();
    rstats.npcs = 0;
    studio_begin_frame(frame_index);
    draw_npcs(eye);
    uint32_t t5b = TICKS_READ();
    rstats.t_bents += TICKS_DISTANCE(t5, t5a);
    rstats.t_npcs += TICKS_DISTANCE(t5a, t5b);
#ifndef NOWHOOK
    {
        // projectiles and the first-person weapon (weapons.c)
        extern void weapons_draw3d(const float *eye, const float *angles, const uint8_t *light);
        extern void pm_get_origin(float *o);
        float po[3];
        uint8_t lt[3];
        pm_get_origin(po);
        bsp_light_at(po, lt);
        t3d_state_set_drawflags(T3D_FLAG_DEPTH | T3D_FLAG_TEXTURED | T3D_FLAG_SHADED | NPC_CULL);
        bound_tex = -1;
        weapons_draw3d(eye, angles, lt);
        t3d_state_set_drawflags(T3D_FLAG_DEPTH | T3D_FLAG_TEXTURED | T3D_FLAG_SHADED);
        bound_tex = -1;
        bound_alpha = -1;
    }
#endif
    uint32_t t6 = TICKS_READ();
    rstats.t_sky += TICKS_DISTANCE(t3, t4);
    rstats.t_world += TICKS_DISTANCE(t4, t5);
    rstats.t_ents += TICKS_DISTANCE(t5, t6);
    flush_cache();

    // 2D overlay
    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    if (hud) hud();
    rdpq_detach_show();
}

T3DViewport *render_viewport(void) { return &viewport; }

// release everything that belongs to the current map (call before loading the next one)
void render_free_map(void)
{
    rspq_wait();
    if (sky_block) { rspq_block_free(sky_block); sky_block = NULL; }
    sky_ok = false;
    free_model_blocks();
    free_texture_blocks();
    free(chunk_strips); chunk_strips = NULL;
    free(strips); strips = NULL;
    free(strip_idx); strip_idx = NULL;
    free(face_visframe); face_visframe = NULL;
    free(tex_chain); tex_chain = NULL;
    free(face_next); face_next = NULL;
    free(visleaf_list); visleaf_list = NULL;
    free(leaf_inpvs); leaf_inpvs = NULL;
    free(vis_faces); vis_faces = NULL;
    free(face_box); face_box = NULL;
    free(ent_leafs); ent_leafs = NULL;
}

