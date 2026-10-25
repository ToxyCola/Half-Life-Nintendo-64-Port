// HL64 - studio model loading, skeletal animation and skinned drawing.
// Bone math follows Valve's StudioModelRenderer (QuaternionMatrix, parent concatenation);
// each bone's vertices are transformed on the RSP with that bone's matrix.
#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include "studio.h"
#include "render.h"

#define MAX_MODELS 64
#define MAX_DRAWN_BONES 768         // per frame, all NPCs together
#define FB_COUNT 3

static studio_t *cache[MAX_MODELS];
static int num_cached;
static T3DMat4FP *bone_pool;         // FB_COUNT * MAX_DRAWN_BONES, uncached
// poses that never change (bodies on the floor): their bone matrices are kept here once computed
#define STATIC_BONES 1024
static T3DMat4FP *static_pool;
static int static_used;
static int pool_frame, pool_used;
// shared fixed poses (gibs, mines, props, bodies on a key frame): model-local bone matrices are the
// same for every instance, so they are computed once per map and kept in static_pool
#define MAX_POSES 64
static struct { studio_t *m; int16_t seq, frame, off, natt; float att[4][3]; } poses[MAX_POSES];
static int num_poses;

static void *lump(uint8_t *buf, uint32_t *tab, int i, int *count)
{
    if (count) *count = tab[i * 2 + 1];
    return buf + tab[i * 2];
}

static void record_textures(studio_t *m)
{
    m->texblocks = calloc(m->numtex, sizeof(void *));
    for (int t = 0; t < m->numtex; t++) {
        stex_t *tx = &m->tex[t];
        rspq_block_begin();
        rdpq_tex_upload_tlut((uint16_t *)(m->texdata + tx->pofs), 0, 256);
        surface_t surf = surface_make_linear(m->texdata + tx->dofs, FMT_CI8, tx->tw, tx->th);
        rdpq_tex_upload(TILE0, &surf, &(rdpq_texparms_t){.s.repeats = REPEAT_INFINITE, .t.repeats = REPEAT_INFINITE});
        m->texblocks[t] = rspq_block_end();
    }
}

// ---- per-map animation filter: characters carry every animation any map's scenes use, but a
// map only needs its own scenes' ones plus what the AI plays. The rest is dropped at load time.
static const char **filter_names;
static int filter_count;
static const int *filter_acts;
static int filter_nacts;
void studio_set_filter(const char **names, int count, const int *acts, int nacts)
{
    filter_names = names; filter_count = count; filter_acts = acts; filter_nacts = nacts;
}

static bool seq_wanted(sseq_t *s, int idx)
{
    if (idx == 0) return true;
    for (int i = 0; i < filter_nacts; i++) if (s->activity == filter_acts[i]) return true;
    for (int i = 0; i < filter_count; i++) {
        const char *n = filter_names[i];
        if (n[0] == '#') { if (atoi(n + 1) == s->pad) return true; }
        else if (!strcasecmp(n, s->name)) return true;
    }
    return false;
}

static int seq_anim_bytes(int nb, const uint8_t *abase, int nf)
{
    int nmove = 0;
    for (int b = 0; b < nb; b++) nmove += abase[b];
    return ((nb + 1) & ~1) + nb * 6 + nf * (nb + nmove) * 6;
}

// drop unneeded sequences' frames from the anim lump and shrink the buffer (before any pointer
// into it is taken); dropped sequences play sequence 0 instead
static uint8_t *compact_anims(uint8_t *buf, int *size)
{
    uint32_t *h = (uint32_t *)buf;
    uint32_t *tab = (uint32_t *)(buf + 56);
    int numbones = tab[0 * 2 + 1];
    int numseqs = tab[1 * 2 + 1];
    if (!filter_names || numseqs <= 12) return buf;
    (void)h;
    sseq_t *seqs = (sseq_t *)(buf + tab[1 * 2]);
    uint32_t anim_ofs = tab[2 * 2];
    // end of the anim lump: the next lump in the file
    uint32_t anim_end = *size;
    for (int i = 0; i < (int)h[4]; i++) if (tab[i * 2] > anim_ofs && tab[i * 2] < anim_end) anim_end = tab[i * 2];
    uint8_t *anim = buf + anim_ofs;
    bool keep[256];
    int newsize = 0;
    for (int i = 0; i < numseqs && i < 256; i++) {
        keep[i] = seq_wanted(&seqs[i], i);
        if (keep[i]) newsize += (seq_anim_bytes(numbones, anim + seqs[i].animofs, seqs[i].numframes) + 1) & ~1;
    }
    newsize = (newsize + 15) & ~15;
    int oldsize = anim_end - anim_ofs;
    int delta = (oldsize - newsize) & ~15;
    if (delta < 4096) return buf;
    uint8_t *tmp = memalign(16, newsize);
    if (!tmp) return buf;
    int at = 0;
    uint32_t ofs0 = 0;
    for (int i = 0; i < numseqs && i < 256; i++) {
        if (!keep[i]) continue;
        int n = seq_anim_bytes(numbones, anim + seqs[i].animofs, seqs[i].numframes);
        memcpy(tmp + at, anim + seqs[i].animofs, n);
        seqs[i].animofs = at;
        if (i == 0) ofs0 = at;
        at += (n + 1) & ~1;
    }
    for (int i = 0; i < numseqs && i < 256; i++) {
        if (keep[i]) continue;
        seqs[i].animofs = ofs0;                       // sequence 0's frames
        seqs[i].numframes = seqs[0].numframes;
        seqs[i].numevents = 0;
    }
    // move the tail down over the freed space, put the compacted frames in, shrink
    int keepsize = oldsize - delta;                   // >= newsize, 16-aligned difference
    memmove(buf + anim_ofs + keepsize, buf + anim_end, *size - anim_end);
    memcpy(buf + anim_ofs, tmp, at);
    free(tmp);
    for (int i = 0; i < (int)h[4]; i++) if (tab[i * 2] > anim_ofs) tab[i * 2] -= delta;
    *size -= delta;
    uint8_t *nb = realloc(buf, *size);
    return nb ? nb : buf;
}

// triangle lists -> tiny3d strips (as the world's chunks, render.c build_strips): one command per
// strip instead of one per triangle keeps the RSP command queue from filling up behind characters.
// Two passes (count, then fill) so the only allocations are the final, exact-size ones.
static void strip_pass(studio_t *m, int16_t *idx, uint32_t *st, uint16_t *cf, uint16_t *cc, int *out_nidx, int *out_nst)
{
    int nidx = 0, nst = 0;
    for (int c = 0; c < m->numchunks; c++) {
        schunk_t *ch = &m->chunks[c];
        const uint8_t *tri = m->tris + ch->firsttri * 3;
        int nv = 0;
        for (int i = 0; i < ch->numtris * 3; i++) if (tri[i] + 1 > nv) nv = tri[i] + 1;
        int limit = ((T3D_VERTEX_CACHE_SIZE - nv) * 36) / 2 - 8;
        if (limit > 240) limit = 240;
        if (limit < 6) limit = 6;
        if (cf) { cf[c] = nst; cc[c] = 0; }
        int start = -1, a = -1, b = -1;
#define PUT(v) do { if (idx) idx[nidx] = (v); nidx++; } while (0)
        for (int i = 0; i < ch->numtris; i++) {
            int t0 = tri[i * 3], t1 = tri[i * 3 + 1], t2 = tri[i * 3 + 2];
            int ext = -1;
            if (start >= 0 && nidx - start + 1 <= limit) {
                int v[3] = {t0, t1, t2};
                for (int k = 0; k < 3; k++) {
                    int x = v[k], y = v[(k + 1) % 3], z = v[(k + 2) % 3];
                    if ((y == a && z == b) || (y == b && z == a)) { ext = x; break; }
                }
            }
            if (ext >= 0) { PUT(ext); a = b; b = ext; continue; }
            if (start < 0 || nidx - start + 3 > limit) {
                if (start >= 0) { if (st) st[nst - 1] |= (nidx - start); while (nidx & 3) PUT(0); }
                start = nidx;
                if (st) st[nst] = (uint32_t)nidx << 8;
                nst++;
                if (cc) cc[c]++;
                PUT(t0); PUT(t1); PUT(t2);
            } else {
                PUT((int16_t)(t0 | 0x8000)); PUT(t1); PUT(t2);
            }
            a = t1; b = t2;
        }
        if (start >= 0) { if (st) st[nst - 1] |= (nidx - start); while (nidx & 3) PUT(0); }
#undef PUT
    }
    *out_nidx = nidx; *out_nst = nst;
}
static void build_model_strips(studio_t *m)
{
    m->strip_idx = NULL; m->strip_ofs = NULL; m->cs_first = m->cs_count = NULL;
    int nidx, nst;
    strip_pass(m, NULL, NULL, NULL, NULL, &nidx, &nst);
    int16_t *idx = memalign(16, nidx * sizeof(int16_t) + 16);
    uint32_t *st = malloc(nst * sizeof(uint32_t) + 4);
    uint16_t *cf = malloc(m->numchunks * sizeof(uint16_t) * 2 + 4);
    if (!idx || !st || !cf) { free(idx); free(st); free(cf); return; }
    uint16_t *cc = cf + m->numchunks;
    strip_pass(m, idx, st, cf, cc, &nidx, &nst);
    for (int i = 0; i < nst; i++) t3d_indexbuffer_convert(&idx[st[i] >> 8], st[i] & 0xff);
    data_cache_hit_writeback(idx, nidx * sizeof(int16_t));
    m->strip_idx = idx; m->strip_ofs = st; m->cs_first = cf; m->cs_count = cc;
    debugf("studio strips: %s %d indices %d strips (%d bytes)\n", m->name, nidx, nst, (int)(nidx * 2 + nst * 4 + m->numchunks * 4));
}

studio_t *studio_get(const char *name)
{
    for (int i = 0; i < num_cached; i++)
        if (!strcasecmp(cache[i]->name, name)) return cache[i];
    if (num_cached >= MAX_MODELS) return NULL;
    char path[80];
    snprintf(path, sizeof(path), "rom:/models/%s.hlm", name);
    FILE *f = fopen(path, "rb");
    if (!f) { debugf("studio: no model %s\n", name); return NULL; }
    fseek(f, 0, SEEK_END);
    int csize = ftell(f);
    fclose(f);
    // models loaded during play (a dropped gun, a weapon's view model): don't run out of memory
    // over one, the game carries on without drawing it
    {
        void *volatile probe = malloc(csize * 6 + 32 * 1024);
        if (!probe) { debugf("studio: no room for %s\n", name); return NULL; }
        free(probe);
    }
    int size;
    uint8_t *buf = asset_load(path, &size);
    if (!buf || memcmp(buf, "HLM1", 4)) { free(buf); return NULL; }
    // textures and palettes are read by the RDP straight from this buffer: it must be 8-byte
    // aligned or the real RDP locks up (asset_load can hand back a 4-byte-aligned block)
    if ((uintptr_t)buf & 15) {
        uint8_t *a = memalign(16, size);
        if (!a) { free(buf); return NULL; }
        memcpy(a, buf, size);
        free(buf);
        buf = a;
    }
    if (strncmp(name, "v_", 2)) buf = compact_anims(buf, &size);
    studio_t *m = calloc(1, sizeof(studio_t));
    strncpy(m->name, name, sizeof(m->name) - 1);
    m->base = buf;
    uint32_t *h = (uint32_t *)buf;
    m->posscale = h[1];
    m->numskinref = h[2];
    m->numskinfamilies = h[3];
    float *fl = (float *)(buf + 20);
    memcpy(m->eye, fl, 12); memcpy(m->hullmin, fl + 3, 12); memcpy(m->hullmax, fl + 6, 12);
    uint32_t *tab = (uint32_t *)(buf + 56);
    int dummy;
    m->parents = lump(buf, tab, 0, &m->numbones);
    m->seqs = lump(buf, tab, 1, &m->numseqs);
    m->anim = lump(buf, tab, 2, NULL);
    m->tex = lump(buf, tab, 3, &m->numtex);
    m->texdata = lump(buf, tab, 4, NULL);
    m->skins = lump(buf, tab, 5, NULL);
    m->bodyparts = lump(buf, tab, 6, &m->numbodyparts);
    m->submodels = lump(buf, tab, 7, &dummy);
    m->chunks = lump(buf, tab, 8, &m->numchunks);
    m->loads = lump(buf, tab, 9, &dummy);
    m->verts = lump(buf, tab, 10, &dummy);
    m->tris = lump(buf, tab, 11, &dummy);
    m->events = lump(buf, tab, 12, &dummy);
    m->mouth_bone = -1;
    if (h[4] > 13) {
        int has = 0;
        uint8_t *c = lump(buf, tab, 13, &has);
        if (has) {
            m->mouth_bone = *(int16_t *)c;
            m->mouth_type = *(uint16_t *)(c + 2);
            m->mouth_start = *(float *)(c + 4);
            m->mouth_end = *(float *)(c + 8);
        }
    }
    m->numatt = 0; m->att = NULL;
    if (h[4] > 14) m->att = lump(buf, tab, 14, &m->numatt);
    m->ctl0_bone = -1;
    if (h[4] > 15) {
        int has = 0;
        uint8_t *c = lump(buf, tab, 15, &has);
        if (has) {
            m->ctl0_bone = *(int16_t *)c;
            m->ctl0_type = *(uint16_t *)(c + 2);
            m->ctl0_start = *(float *)(c + 4);
            m->ctl0_end = *(float *)(c + 8);
        }
    }
    data_cache_hit_writeback(buf, size);
    record_textures(m);
    build_model_strips(m);
    cache[num_cached++] = m;
    debugf("studio: %s %d bones %d seqs %d chunks (%d KB) at %p tex %p\n", name, m->numbones, m->numseqs, m->numchunks, size / 1024, buf, m->texdata);
    return m;
}

// drop one model (the previous weapon's view model when switching weapons)
void studio_free(studio_t *m)
{
    if (!m) return;
    for (int i = 0; i < num_cached; i++) {
        if (cache[i] != m) continue;
        rspq_wait();
        for (int t = 0; t < m->numtex; t++) if (m->texblocks[t]) rspq_block_free(m->texblocks[t]);
        free(m->texblocks);
        free(m->strip_idx); free(m->strip_ofs); free(m->cs_first);
        free(m->base);
        free(m);
        for (int p = 0; p < num_poses; p++) if (poses[p].m == m) poses[p].m = NULL;
        cache[i] = cache[--num_cached];
        return;
    }
}

void studio_free_all(void)
{
    rspq_wait();
    static_used = 0;
    num_poses = 0;
    for (int i = 0; i < num_cached; i++) {
        studio_t *m = cache[i];
        for (int t = 0; t < m->numtex; t++) if (m->texblocks[t]) rspq_block_free(m->texblocks[t]);
        free(m->texblocks);
        free(m->strip_idx); free(m->strip_ofs); free(m->cs_first);
        free(m->base);
        free(m);
    }
    num_cached = 0;
}

int studio_find_seq(studio_t *m, const char *name)
{
    if (!m || !name) return -1;
    for (int i = 0; i < m->numseqs; i++)
        if (!strcasecmp(m->seqs[i].name, name)) return i;
    return -1;
}

int studio_find_activity(studio_t *m, int activity)
{
    if (!m) return -1;
    for (int i = 0; i < m->numseqs; i++)
        if (m->seqs[i].activity == activity) return i;
    return -1;
}

float studio_seq_duration(studio_t *m, int seq)
{
    if (!m || seq < 0 || seq >= m->numseqs) return 0;
    sseq_t *s = &m->seqs[seq];
    float fps = s->fps > 0 ? s->fps : 10;
    return s->numframes > 1 ? (s->numframes - 1) / fps : 0.1f;
}

void studio_begin_frame(int frame_index)
{
    if (!bone_pool) bone_pool = malloc_uncached(sizeof(T3DMat4FP) * MAX_DRAWN_BONES * FB_COUNT);
    if (!static_pool) static_pool = malloc_uncached(sizeof(T3DMat4FP) * STATIC_BONES);
    pool_frame = frame_index % FB_COUNT;
    pool_used = 0;
}

// ---------------------------------------------------------------- bones
static void quat_to_mat(const float *q, const float *p, float m[3][4])
{
    // QuaternionMatrix() from Valve's studio_util.cpp, with translation in column 3
    m[0][0] = 1.0f - 2.0f * q[1] * q[1] - 2.0f * q[2] * q[2];
    m[1][0] = 2.0f * q[0] * q[1] + 2.0f * q[3] * q[2];
    m[2][0] = 2.0f * q[0] * q[2] - 2.0f * q[3] * q[1];
    m[0][1] = 2.0f * q[0] * q[1] - 2.0f * q[3] * q[2];
    m[1][1] = 1.0f - 2.0f * q[0] * q[0] - 2.0f * q[2] * q[2];
    m[2][1] = 2.0f * q[1] * q[2] + 2.0f * q[3] * q[0];
    m[0][2] = 2.0f * q[0] * q[2] + 2.0f * q[3] * q[1];
    m[1][2] = 2.0f * q[1] * q[2] - 2.0f * q[3] * q[0];
    m[2][2] = 1.0f - 2.0f * q[0] * q[0] - 2.0f * q[1] * q[1];
    m[0][3] = p[0]; m[1][3] = p[1]; m[2][3] = p[2];
}

static void concat(const float a[3][4], const float b[3][4], float o[3][4])
{
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++)
            o[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
        o[r][3] += a[r][3];
    }
}

void studio_draw(const studio_draw_t *d)
{
    studio_t *m = d->model;
    if (!m || m->numseqs == 0) return;
    bool cached = d->cache_slot && *d->cache_slot >= 0 && *d->cache_slot + m->numbones <= static_used;
    int seq = d->seq;
    if (seq < 0 || seq >= m->numseqs) seq = 0;
    sseq_t *s = &m->seqs[seq];
    int nf = s->numframes;
    float fr = d->frame;
    if (fr < 0) fr = 0;
    if (fr > nf - 1) fr = nf - 1;
    int f0 = (int)fr, f1 = f0 + 1;
    if (f1 >= nf) f1 = (s->flags & 1) ? 0 : nf - 1;
    float t = fr - f0;
    // a pose on a key frame with no controllers is shared by every instance
    int pose = -1;
    // (only poses that stay put: a one-frame or finished sequence, or one the caller says won't change;
    // animating characters passing a key frame would just fill the pool)
    bool keyed = !cached && (f0 == f1 || (d->cacheable && t < 0.01f)) && d->ctl0 == 0 && d->mouth <= 0 && !d->use_xform && m->numbones <= 128;
    if (keyed) {
        for (int i = 0; i < num_poses; i++)
            if (poses[i].m == m && poses[i].seq == seq && poses[i].frame == f0) { pose = i; break; }
        if (pose >= 0) cached = true;
    }
    // keep room for the first-person weapon (drawn last): busy scenes used to run the pool dry
    if (pool_used + (cached ? 0 : m->numbones) + 1 > MAX_DRAWN_BONES - (d->use_xform ? 0 : 160)) return;
    // compact keys (tools/mdl2n64.py): position mask, constant positions, then per frame
    // x,y,z of each bone's quaternion followed by the positions of the moving bones
    const uint8_t *abase = m->anim + s->animofs;
    const uint8_t *posmask = abase;
    const int16_t *cpos = (const int16_t *)(abase + ((m->numbones + 1) & ~1));
    const int16_t *fdata = cpos + m->numbones * 3;
    int nmove = 0;
    for (int b = 0; b < m->numbones; b++) nmove += posmask[b];
    int stride = (m->numbones + nmove) * 3;
    const int16_t *k0 = fdata + f0 * stride, *k1 = fdata + f1 * stride;
    const int16_t *p0 = k0 + m->numbones * 3, *p1 = k1 + m->numbones * 3;

    // entity transform: yaw + origin (studio models turn with the entity's yaw)
    float cy = cosf(d->yaw * (float)M_PI / 180), sy = sinf(d->yaw * (float)M_PI / 180);
    float ent[3][4] = {{cy, -sy, 0, d->origin[0]}, {sy, cy, 0, d->origin[1]}, {0, 0, 1, d->origin[2]}};
    if (d->use_xform) memcpy(ent, d->xform, sizeof(ent));
    // the entity transform is its own matrix on the stack: bone matrices stay model-local (shareable)
    T3DMat4FP *emat = &bone_pool[pool_frame * MAX_DRAWN_BONES + pool_used++];
    {
        T3DMat4 tm;
        for (int r = 0; r < 3; r++) { tm.m[0][r] = ent[r][0]; tm.m[1][r] = ent[r][1]; tm.m[2][r] = ent[r][2]; tm.m[3][r] = ent[r][3]; }
        tm.m[0][3] = tm.m[1][3] = tm.m[2][3] = 0;
        tm.m[3][3] = 1;
        t3d_mat4_to_fixed_3x4(emat, &tm);
    }
    float bonemat[128][3][4];
    float inv = 1.0f / m->posscale;
    T3DMat4FP *out = pose >= 0 ? &static_pool[poses[pose].off]
                   : cached ? &static_pool[*d->cache_slot] : &bone_pool[pool_frame * MAX_DRAWN_BONES + pool_used];
    if (pose >= 0) {
        for (int a = 0; a < d->att_n && a < poses[pose].natt; a++) {
            const float *la = poses[pose].att[a];
            for (int r = 0; r < 3; r++) d->att_out[a][r] = ent[r][0] * la[0] + ent[r][1] * la[1] + ent[r][2] * la[2] + ent[r][3];
        }
    }
#ifdef PROFX
    uint32_t pb0 = TICKS_READ();
#endif
    if (!cached) {
    for (int b = 0; b < m->numbones && b < 128; b++) {
        float qa[4], qb[4], q[4], p[3], dot = 0;
        const float qs = 1.0f / 32767.0f;
        for (int i = 0; i < 3; i++) { qa[i] = k0[b * 3 + i] * qs; qb[i] = k1[b * 3 + i] * qs; }
        float wa = 1 - qa[0] * qa[0] - qa[1] * qa[1] - qa[2] * qa[2];
        qa[3] = wa > 0 ? sqrtf(wa) : 0;
        if (t < 0.01f || f0 == f1) {
            // on a key (paused, static or last frame): the stored quaternion is already unit length
            for (int i = 0; i < 4; i++) q[i] = qa[i];
        } else {
            float wb = 1 - qb[0] * qb[0] - qb[1] * qb[1] - qb[2] * qb[2];
            qb[3] = wb > 0 ? sqrtf(wb) : 0;
            for (int i = 0; i < 4; i++) dot += qa[i] * qb[i];
            float sgn = dot < 0 ? -1.0f : 1.0f;
            float len = 0;
            for (int i = 0; i < 4; i++) {
                q[i] = qa[i] * (1 - t) + sgn * qb[i] * t;
                len += q[i] * q[i];
            }
            len = 1.0f / sqrtf(len > 0 ? len : 1);
            for (int i = 0; i < 4; i++) q[i] *= len;
        }
        if (posmask[b]) {
            for (int i = 0; i < 3; i++) p[i] = (p0[i] * (1 - t) + p1[i] * t) * inv;
            p0 += 3; p1 += 3;
        } else {
            for (int i = 0; i < 3; i++) p[i] = cpos[b * 3 + i] * inv;
        }
        if (b == m->ctl0_bone && d->ctl0 != 0) {
            float lo = fminf(m->ctl0_start, m->ctl0_end), hi = fmaxf(m->ctl0_start, m->ctl0_end);
            float v = fminf(fmaxf(d->ctl0, lo), hi);
            p[(m->ctl0_type & 1) ? 0 : (m->ctl0_type & 2) ? 1 : 2] += v;
        }
        if (b == m->mouth_bone && d->mouth > 0) {
            // add the jaw angle about the controller's axis (STUDIO_XR/YR/ZR)
            float ang = (m->mouth_start + (m->mouth_end - m->mouth_start) * d->mouth * 0.5f) * (float)M_PI / 360.0f;   // half the PC range: less gaping   // jaw opens downward
            float r[4] = {0, 0, 0, cosf(ang)}, sa = sinf(ang), o[4];
            r[(m->mouth_type & 0x08) ? 0 : (m->mouth_type & 0x10) ? 1 : 2] = sa;
            // HL adds the controller to the bone's euler angle (q = Rz * Ry * Rx): for Z that is a
            // rotation in the parent frame (r * q), for X one in the bone's own frame (q * r)
            const float *A = r, *B = q;
            if (m->mouth_type & 0x08) { A = q; B = r; }
            o[0] = A[3] * B[0] + A[0] * B[3] + A[1] * B[2] - A[2] * B[1];
            o[1] = A[3] * B[1] - A[0] * B[2] + A[1] * B[3] + A[2] * B[0];
            o[2] = A[3] * B[2] + A[0] * B[1] - A[1] * B[0] + A[2] * B[3];
            o[3] = A[3] * B[3] - A[0] * B[0] - A[1] * B[1] - A[2] * B[2];
            memcpy(q, o, 16);
        }
        float local[3][4];
        quat_to_mat(q, p, local);
        int par = m->parents[b];
        if (par < 0) memcpy(bonemat[b], local, sizeof(local));
        else concat(bonemat[par], local, bonemat[b]);
        // to tiny3d: column-major 4x4, vertex positions are scaled by posscale
        T3DMat4 tm;
        for (int r = 0; r < 3; r++) {
            tm.m[0][r] = bonemat[b][r][0] * inv;
            tm.m[1][r] = bonemat[b][r][1] * inv;
            tm.m[2][r] = bonemat[b][r][2] * inv;
            tm.m[3][r] = bonemat[b][r][3];
        }
        tm.m[0][3] = tm.m[1][3] = tm.m[2][3] = 0;
        tm.m[3][3] = 1;
        t3d_mat4_to_fixed_3x4(&out[b], &tm);
    }
    pool_used += m->numbones;
    float latt[4][3];
    int natt = 0;
    for (int a = 0; a < m->numatt && a < 4; a++) {
        satt_t *at = &m->att[a];
        if (at->bone < 0 || at->bone >= m->numbones || at->bone >= 128) break;
        float (*bm)[4] = bonemat[at->bone];
        for (int r = 0; r < 3; r++) latt[a][r] = bm[r][0] * at->org[0] + bm[r][1] * at->org[1] + bm[r][2] * at->org[2] + bm[r][3];
        natt = a + 1;
    }
    for (int a = 0; a < d->att_n && a < natt; a++)
        for (int r = 0; r < 3; r++) d->att_out[a][r] = ent[r][0] * latt[a][0] + ent[r][1] * latt[a][1] + ent[r][2] * latt[a][2] + ent[r][3];
    if (keyed && num_poses < MAX_POSES && static_pool && static_used + m->numbones <= STATIC_BONES) {
        memcpy(&static_pool[static_used], out, sizeof(T3DMat4FP) * m->numbones);
        poses[num_poses].m = m; poses[num_poses].seq = seq; poses[num_poses].frame = f0;
        poses[num_poses].off = static_used; poses[num_poses].natt = natt;
        memcpy(poses[num_poses].att, latt, sizeof(latt));
        num_poses++;
        static_used += m->numbones;
    } else
    // a pose that won't change: keep its matrices for the next frames
    if (d->cache_slot && d->cacheable && static_pool && static_used + m->numbones <= STATIC_BONES) {
        memcpy(&static_pool[static_used], out, sizeof(T3DMat4FP) * m->numbones);
        *d->cache_slot = static_used;
        static_used += m->numbones;
    }
    }
#ifdef PROFX
    uint32_t pb1 = TICKS_READ();
#endif

    // lighting: ambient from the floor's baked light + a soft light from above
    uint8_t amb[4] = {(uint8_t)(d->light[0] * 0.75f), (uint8_t)(d->light[1] * 0.75f), (uint8_t)(d->light[2] * 0.75f), 0xff};
    uint8_t dir[4] = {(uint8_t)(d->light[0] * 0.5f), (uint8_t)(d->light[1] * 0.5f), (uint8_t)(d->light[2] * 0.5f), 0xff};
    t3d_light_set_ambient(amb);
    T3DVec3 ldir = {{0.3f, 0.2f, 0.93f}};
    t3d_light_set_directional(0, dir, &ldir);
    t3d_light_set_count(1);

    int family = (d->skin >= 0 && d->skin < m->numskinfamilies) ? d->skin : 0;
    int cur_tex = -1, cur_alpha = -1;
    t3d_matrix_push(emat);
    t3d_matrix_push_pos(1);
    for (int bp = 0; bp < m->numbodyparts; bp++) {
        sbodypart_t *part = &m->bodyparts[bp];
        int sub = part->nummodels ? (d->body / (part->base ? part->base : 1)) % part->nummodels : 0;
        ssubmodel_t *sm = &m->submodels[part->firstmodel + sub];
        for (uint32_t c = sm->firstchunk; c < sm->firstchunk + sm->numchunks; c++) {
            schunk_t *ch = &m->chunks[c];
            int tex = ch->skinref;
            if (m->numskinref) tex = m->skins[family * m->numskinref + ch->skinref];
            if (tex != cur_tex && tex >= 0 && tex < m->numtex) {
                int alpha = (m->tex[tex].flags & 0x40) ? 1 : 0;
                if (alpha != cur_alpha) { rdpq_mode_alphacompare(alpha ? 128 : 0); cur_alpha = alpha; }
                rspq_block_run(m->texblocks[tex]);
                cur_tex = tex;
            }
            for (int l = 0; l < ch->numloads; l++) {
                sload_t *ld = &m->loads[ch->firstload + l];
                t3d_matrix_set(&out[ld->bone], true);
                t3d_vert_load(&m->verts[ld->vofs >> 1], ld->cofs, ld->cnt);
            }
            if (m->strip_idx) {
                for (int k = 0; k < m->cs_count[c]; k++) {
                    uint32_t so = m->strip_ofs[m->cs_first[c] + k];
                    t3d_tri_draw_strip(&m->strip_idx[so >> 8], so & 0xff);
                }
            } else {
                const uint8_t *tri = m->tris + ch->firsttri * 3;
                for (int i = 0; i < ch->numtris; i++, tri += 3) t3d_tri_draw(tri[0], tri[1], tri[2]);
            }
            t3d_tri_sync();
            rstats.tris += ch->numtris;
        }
    }
    t3d_matrix_pop(2);
#ifdef PROFX
    { static uint32_t ab, as; static int an, nb; ab += TICKS_DISTANCE(pb0, pb1); as += TICKS_DISTANCE(pb1, TICKS_READ()); nb += m->numbones;
      if (++an == 120) { debugf("  studio: bones %d us/model (%d bones avg), submit %d us/model\n", (int)TICKS_TO_US(ab / an), nb / an, (int)TICKS_TO_US(as / an)); ab = as = an = nb = 0; } }
#endif
    if (cur_alpha == 1) rdpq_mode_alphacompare(0);
    uint8_t white[4] = {0xff, 0xff, 0xff, 0xff};
    t3d_light_set_ambient(white);
    t3d_light_set_count(0);
}
