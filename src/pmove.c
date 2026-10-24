// HL64 - engine side of Valve's player movement (pm_shared.c).
// Valve's code is used unmodified; this file supplies the engine callbacks that
// the PC engine (hw.dll) normally provides: hull traces, contents, sounds, files.
#include <libdragon.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "mathlib.h"
#include "const.h"
#include "usercmd.h"
#include "pm_defs.h"
#include "pm_shared.h"
#include "pm_movevars.h"
#include "bsp.h"
#include "game.h"
#include "npc.h"

#define CONTENTS_CURRENT_0    -9
#define CONTENTS_CURRENT_DOWN -14
#ifndef CONTENTS_LADDER
#define CONTENTS_LADDER -16
#endif
#define VectorSet(v, x, y, z) ((v)[0] = (x), (v)[1] = (y), (v)[2] = (z))

playermove_t pmove_data;
movevars_t movevars;
static playermove_t *pm = &pmove_data;

// pmove->usehull (0 stand,1 duck,2 point,3 large) -> BSP hull index
static const int usehull_to_bsp[4] = {1, 3, 0, 2};

// Box hull for monsters (SV_InitBoxHull / SV_HullForBox from the GoldSrc server)
static bclipnode_t box_clipnodes[6];
static bplane_t box_planes[6];
static hull_t box_hull;

static void init_box_hull(void)
{
    box_hull.clipnodes = box_clipnodes;
    box_hull.planes = box_planes;
    box_hull.firstclipnode = 0;
    box_hull.lastclipnode = 5;
    for (int i = 0; i < 6; i++) {
        int side = i & 1;
        box_clipnodes[i].planenum = i;
        box_clipnodes[i].children[side] = CONTENTS_EMPTY;
        box_clipnodes[i].children[side ^ 1] = (i != 5) ? i + 1 : CONTENTS_SOLID;
        memset(&box_planes[i], 0, sizeof(bplane_t));
        box_planes[i].type = i >> 1;
        box_planes[i].normal[i >> 1] = 1;
    }
}

static hull_t *hull_for_box(const float *mins, const float *maxs)
{
    box_planes[0].dist = maxs[0]; box_planes[1].dist = mins[0];
    box_planes[2].dist = maxs[1]; box_planes[3].dist = mins[1];
    box_planes[4].dist = maxs[2]; box_planes[5].dist = mins[2];
    return &box_hull;
}

static hull_t *hull_for_physent(physent_t *pe, int usehull, float *offset)
{
    VectorCopy(pe->origin, offset);
    if (!pe->model) {
        // expand the monster box by the player hull (SV_HullForEntity)
        float mn[3], mx[3];
        int h = usehull & 3;
        for (int k = 0; k < 3; k++) {
            mn[k] = pe->mins[k] - pm->player_maxs[h][k];
            mx[k] = pe->maxs[k] - pm->player_mins[h][k];
        }
        return hull_for_box(mn, mx);
    }
    model_t *m = pe->model;
    return &m->hulls[usehull_to_bsp[usehull & 3]];
}

static bool has_angles(physent_t *pe)
{
    return pe->angles[0] != 0 || pe->angles[1] != 0 || pe->angles[2] != 0;
}

// trace against one brush physent, handling rotated models like GoldSrc does
static void trace_physent(physent_t *pe, int usehull, const float *start, const float *end, btrace_t *tr)
{
    {   // quick reject: the swept player box doesn't touch the entity's bounds
        int h = usehull & 3;
        float amn[3], amx[3];
        if (pe->model && pe->iuser4) { VectorCopy(pe->mins, amn); VectorCopy(pe->maxs, amx); }
        else if (!pe->model) { for (int k = 0; k < 3; k++) { amn[k] = pe->origin[k] + pe->mins[k]; amx[k] = pe->origin[k] + pe->maxs[k]; } }
        else goto full;
        for (int k = 0; k < 3; k++) {
            float lo = fminf(start[k], end[k]) + pm->player_mins[h][k] - 2, hi = fmaxf(start[k], end[k]) + pm->player_maxs[h][k] + 2;
            if (hi < amn[k] || lo > amx[k]) {
                memset(tr, 0, sizeof(*tr));
                tr->fraction = 1.0f;
                tr->inopen = true;
                VectorCopy(end, tr->endpos);
                return;
            }
        }
    }
full:;
    float offset[3], s[3], e[3];
    hull_t *hull = hull_for_physent(pe, usehull, offset);
    VectorSubtract(start, offset, s);
    VectorSubtract(end, offset, e);
    bool rotated = pe->solid == SOLID_BSP && has_angles(pe);
    float f[3], r[3], u[3];
    if (rotated) {
        AngleVectors(pe->angles, f, r, u);
        float t[3];
        VectorCopy(s, t); s[0] = DotProduct(t, f); s[1] = -DotProduct(t, r); s[2] = DotProduct(t, u);
        VectorCopy(e, t); e[0] = DotProduct(t, f); e[1] = -DotProduct(t, r); e[2] = DotProduct(t, u);
    }
    bsp_trace_hull(hull, s, e, tr);
    if (rotated && tr->fraction != 1.0f) {
        float n[3];
        VectorCopy(tr->normal, n);
        // transpose of the forward/right/up basis
        float ft[3], rt[3], ut[3];
        AngleVectorsTranspose(pe->angles, ft, rt, ut);
        tr->normal[0] = DotProduct(n, ft);
        tr->normal[1] = DotProduct(n, rt);
        tr->normal[2] = DotProduct(n, ut);
    }
    // endpos back to world space by interpolating the original segment
    for (int i = 0; i < 3; i++) tr->endpos[i] = start[i] + tr->fraction * (end[i] - start[i]);
}

static pmtrace_t player_trace_ex(float *start, float *end, int traceFlags, int ignore_pe, int (*ignore)(physent_t *))
{
    pmtrace_t total;
    memset(&total, 0, sizeof(total));
    total.fraction = 1.0f;
    total.ent = -1;
    VectorCopy(end, total.endpos);
    for (int i = 0; i < pm->numphysent; i++) {
        physent_t *pe = &pm->physents[i];
        if (i == ignore_pe) continue;
        if (ignore && ignore(pe)) continue;
        if (!pe->model && pe->solid != SOLID_BBOX) continue;
        if (i > 0 && (traceFlags & PM_WORLD_ONLY)) break;
        if ((traceFlags & PM_GLASS_IGNORE) && pe->rendermode != 0) continue;
        btrace_t tr;
        trace_physent(pe, pm->usehull, start, end, &tr);
        if (tr.allsolid) tr.startsolid = true;
        if (tr.startsolid) tr.fraction = 0;
        if (tr.fraction < total.fraction || (tr.startsolid && !total.startsolid)) {
            total.allsolid = tr.allsolid;
            total.startsolid = tr.startsolid;
            total.inopen = tr.inopen;
            total.inwater = tr.inwater;
            total.fraction = tr.fraction;
            VectorCopy(tr.endpos, total.endpos);
            VectorCopy(tr.normal, total.plane.normal);
            total.plane.dist = tr.dist;
            total.ent = i;
        }
    }
    return total;
}

static pmtrace_t PM_PlayerTraceImpl(float *start, float *end, int traceFlags, int ignore_pe)
{
    return player_trace_ex(start, end, traceFlags, ignore_pe, NULL);
}

static pmtrace_t PM_PlayerTraceExImpl(float *start, float *end, int traceFlags, int (*pfnIgnore)(physent_t *pe))
{
    return player_trace_ex(start, end, traceFlags, -1, pfnIgnore);
}

static struct pmtrace_s *PM_TraceLineImpl(float *start, float *end, int flags, int usehull, int ignore_pe)
{
    static pmtrace_t tr;
    int old = pm->usehull;
    pm->usehull = usehull;
    tr = player_trace_ex(start, end, flags == PM_TRACELINE_PHYSENTSONLY ? 0 : 0, ignore_pe, NULL);
    pm->usehull = old;
    return &tr;
}

static struct pmtrace_s *PM_TraceLineExImpl(float *start, float *end, int flags, int usehull, int (*pfnIgnore)(physent_t *pe))
{
    static pmtrace_t tr;
    int old = pm->usehull;
    pm->usehull = usehull;
    tr = player_trace_ex(start, end, 0, -1, pfnIgnore);
    pm->usehull = old;
    return &tr;
}

static int test_position_ex(float *pos, pmtrace_t *ptrace, int (*ignore)(physent_t *))
{
    for (int i = 0; i < pm->numphysent; i++) {
        physent_t *pe = &pm->physents[i];
        if (!pe->model && pe->solid != SOLID_BBOX) continue;
        if (ignore && ignore(pe)) continue;
        float offset[3], test[3];
        hull_t *hull = hull_for_physent(pe, pm->usehull, offset);
        VectorSubtract(pos, offset, test);
        if (pe->solid == SOLID_BSP && has_angles(pe)) {
            float f[3], r[3], u[3], t[3];
            AngleVectors(pe->angles, f, r, u);
            VectorCopy(test, t);
            test[0] = DotProduct(t, f); test[1] = -DotProduct(t, r); test[2] = DotProduct(t, u);
        }
        if (bsp_hull_point_contents(hull, hull->firstclipnode, test) == CONTENTS_SOLID) {
            if (ptrace) {
                memset(ptrace, 0, sizeof(*ptrace));
                ptrace->ent = i;
                ptrace->startsolid = ptrace->allsolid = true;
            }
            return i;
        }
    }
    return -1;
}

static int PM_TestPlayerPositionImpl(float *pos, pmtrace_t *ptrace) { return test_position_ex(pos, ptrace, NULL); }
static int PM_TestPlayerPositionExImpl(float *pos, pmtrace_t *ptrace, int (*f)(physent_t *)) { return test_position_ex(pos, ptrace, f); }

static int PM_HullPointContentsImpl(struct hull_s *hull, int num, float *p)
{
    return bsp_hull_point_contents(hull, num, p);
}

static int PM_PointContentsImpl(float *p, int *truecontents)
{
    int cont = bsp_point_contents(p);
    if (truecontents) *truecontents = cont;
    if (cont <= CONTENTS_CURRENT_0 && cont >= CONTENTS_CURRENT_DOWN) cont = CONTENTS_WATER;
    if (cont == CONTENTS_SOLID) return cont;
    // brush entities that carry contents (func_water etc.) live in moveents
    for (int i = 0; i < pm->nummoveent; i++) {
        physent_t *pe = &pm->moveents[i];
        if (pe->solid != SOLID_NOT || pe->skin == CONTENTS_LADDER || !pe->model) continue;
        float test[3];
        VectorSubtract(p, pe->origin, test);
        hull_t *h = &pe->model->hulls[0];
        if (bsp_hull_point_contents(h, h->firstclipnode, test) != CONTENTS_EMPTY) {
            if (truecontents) *truecontents = pe->skin;
            return pe->skin;
        }
    }
    return cont;
}

static int PM_TruePointContentsImpl(float *p) { return bsp_point_contents(p); }

static void *PM_HullForBspImpl(physent_t *pe, float *offset)
{
    return hull_for_physent(pe, pm->usehull, offset);
}

static float PM_TraceModelImpl(physent_t *pe, float *start, float *end, trace_t *trace)
{
    btrace_t tr;
    int old = pm->usehull;
    trace_physent(pe, 2, start, end, &tr); // point hull, as the engine does for ladders
    pm->usehull = old;
    memset(trace, 0, sizeof(*trace));
    trace->allsolid = tr.allsolid;
    trace->startsolid = tr.startsolid;
    trace->fraction = tr.fraction;
    VectorCopy(tr.endpos, trace->endpos);
    VectorCopy(tr.normal, trace->plane.normal);
    trace->plane.dist = tr.dist;
    return tr.fraction;
}

static int PM_GetModelTypeImpl(struct model_s *mod) { (void)mod; return 0; /* mod_brush */ }

static void PM_GetModelBoundsImpl(struct model_s *mod, float *mins, float *maxs)
{
    VectorCopy(mod->bm->mins, mins);
    VectorCopy(mod->bm->maxs, maxs);
}

static const char *PM_TraceTextureImpl(int ground, float *vstart, float *vend)
{
    if (ground < 0 || ground >= pm->numphysent) return NULL;
    physent_t *pe = &pm->physents[ground];
    if (!pe->model) return NULL;
    return bsp_trace_texture(pe->model->index, pe->origin, vstart, vend);
}

// the only physinfo key single player uses: "slj" = the long jump module (CBasePlayer::SetSuitUpdate / items.cpp)
static const char *PM_Info_ValueForKeyImpl(const char *s, const char *key)
{
    (void)s;
    if (!strcmp(key, "slj")) return player.longjump ? "1" : "0";
    return "";
}
static void PM_ParticleImpl(float *o, int c, float l, int z, int zv) { }
static void Con_NPrintfImpl(int idx, char *fmt, ...) { }
static void Con_DPrintfImpl(char *fmt, ...) { }
static void Con_PrintfImpl(char *fmt, ...) { }
static double Sys_FloatTimeImpl(void) { return TICKS_READ() / (double)TICKS_PER_SECOND; }
static void PM_StuckTouchImpl(int hitent, pmtrace_t *tr) { }
static int32 RandomLongImpl(int32 lo, int32 hi) { if (hi <= lo) return lo; return lo + rand() % (hi - lo + 1); }
static float RandomFloatImpl(float lo, float hi) { return lo + (hi - lo) * (rand() / (float)RAND_MAX); }

static int COM_FileSizeImpl(char *filename)
{
    char path[96];
    snprintf(path, sizeof(path), "rom:/%s", filename);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    int s = ftell(f);
    fclose(f);
    return s;
}

static byte *COM_LoadFileImpl(char *path0, int usehunk, int *plen)
{
    char path[96];
    snprintf(path, sizeof(path), "rom:/%s", path0);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    int s = ftell(f);
    fseek(f, 0, SEEK_SET);
    byte *b = malloc(s + 1);
    fread(b, 1, s, f);
    b[s] = 0;
    fclose(f);
    if (plen) *plen = s;
    return b;
}

static void COM_FreeFileImpl(void *b) { free(b); }

static char *memfgetsImpl(byte *pMemFile, int fileSize, int *pFilePos, char *pBuffer, int bufferSize)
{
    int i = *pFilePos, last;
    if (!pMemFile || !pBuffer || i >= fileSize) return NULL;
    last = fileSize;
    if (last - i > bufferSize - 1) last = i + bufferSize - 1;
    int stop = 0;
    while (i < last && !stop) {
        if (pMemFile[i] == '\n') stop = 1;
        i++;
    }
    if (i == *pFilePos) return NULL;
    int size = i - *pFilePos;
    memcpy(pBuffer, pMemFile + *pFilePos, size);
    if (size < bufferSize) pBuffer[size] = 0;
    *pFilePos = i;
    return pBuffer;
}

static void PM_PlaySoundImpl(int channel, const char *sample, float volume, float attenuation, int fFlags, int pitch)
{
    snd_play(sample, volume, channel);
}

static void PM_PlaybackEventFullImpl(int flags, int clientindex, unsigned short eventindex, float delay, float *origin,
                                     float *angles, float fparam1, float fparam2, int iparam1, int iparam2, int bparam1,
                                     int bparam2) { }

void pm_init(void)
{
    memset(pm, 0, sizeof(*pm));
    memset(&movevars, 0, sizeof(movevars));
    // Half-Life single player defaults (server cvars)
    movevars.gravity = 800;
    movevars.stopspeed = 100;
    movevars.maxspeed = 320;
    movevars.spectatormaxspeed = 500;
    movevars.accelerate = 10;
    movevars.airaccelerate = 10;
    movevars.wateraccelerate = 10;
    movevars.friction = 4;
    movevars.edgefriction = 2;
    movevars.waterfriction = 1;
    movevars.entgravity = 1;
    movevars.bounce = 1;
    movevars.stepsize = 18;
    movevars.maxvelocity = 2000;
    movevars.zmax = 4096;
    movevars.footsteps = 1;
    movevars.rollangle = 0;
    movevars.rollspeed = 0;

    pm->movevars = &movevars;
    pm->server = 1;
    pm->multiplayer = 0;
    pm->runfuncs = 1;
    pm->maxspeed = 320;
    pm->clientmaxspeed = 320;
    pm->gravity = 1;
    pm->friction = 1;
    pm->onground = -1;
    pm->movetype = MOVETYPE_WALK;
    VectorSet(pm->view_ofs, 0, 0, 28);

    static const float hmins[4][3] = {{-16, -16, -36}, {-16, -16, -18}, {0, 0, 0}, {-32, -32, -32}};
    static const float hmaxs[4][3] = {{16, 16, 36}, {16, 16, 18}, {0, 0, 0}, {32, 32, 32}};
    memcpy(pm->player_mins, hmins, sizeof(hmins));
    memcpy(pm->player_maxs, hmaxs, sizeof(hmaxs));

    pm->PM_Info_ValueForKey = PM_Info_ValueForKeyImpl;
    pm->PM_Particle = PM_ParticleImpl;
    pm->PM_TestPlayerPosition = PM_TestPlayerPositionImpl;
    pm->Con_NPrintf = Con_NPrintfImpl;
    pm->Con_DPrintf = Con_DPrintfImpl;
    pm->Con_Printf = Con_PrintfImpl;
    pm->Sys_FloatTime = Sys_FloatTimeImpl;
    pm->PM_StuckTouch = PM_StuckTouchImpl;
    pm->PM_PointContents = PM_PointContentsImpl;
    pm->PM_TruePointContents = PM_TruePointContentsImpl;
    pm->PM_HullPointContents = PM_HullPointContentsImpl;
    pm->PM_PlayerTrace = PM_PlayerTraceImpl;
    pm->PM_TraceLine = PM_TraceLineImpl;
    pm->RandomLong = RandomLongImpl;
    pm->RandomFloat = RandomFloatImpl;
    pm->PM_GetModelType = PM_GetModelTypeImpl;
    pm->PM_GetModelBounds = PM_GetModelBoundsImpl;
    pm->PM_HullForBsp = PM_HullForBspImpl;
    pm->PM_TraceModel = PM_TraceModelImpl;
    pm->COM_FileSize = COM_FileSizeImpl;
    pm->COM_LoadFile = COM_LoadFileImpl;
    pm->COM_FreeFile = COM_FreeFileImpl;
    pm->memfgets = memfgetsImpl;
    pm->PM_PlaySound = PM_PlaySoundImpl;
    pm->PM_TraceTexture = PM_TraceTextureImpl;
    pm->PM_PlaybackEventFull = PM_PlaybackEventFullImpl;
    pm->PM_PlayerTraceEx = PM_PlayerTraceExImpl;
    pm->PM_TestPlayerPositionEx = PM_TestPlayerPositionExImpl;
    pm->PM_TraceLineEx = PM_TraceLineExImpl;

    init_box_hull();
    PM_Init(pm);
}

// ---------------------------------------------------------------- per frame

static void add_physent(physent_t *list, int *count, int max, int entindex, model_t *model,
                        const float *origin, const float *angles, int solid, int skin, int rendermode)
{
    if (*count >= max) return;
    physent_t *pe = &list[(*count)++];
    memset(pe, 0, sizeof(*pe));
    pe->model = model;
    pe->info = entindex;
    pe->solid = solid;
    pe->skin = skin;
    pe->rendermode = rendermode;
    VectorCopy(origin, pe->origin);
    VectorCopy(angles, pe->angles);
    strcpy(pe->name, entindex == 0 ? "world" : "brush");
    // world-space bounds (pm_shared never reads mins/maxs of brush physents): traces that can't
    // reach this entity skip its hull walk
    if (entindex > 0 && model) {
        bmodel_t *bm = &world.bmodels[model - world.models];
        bool rot = angles[0] != 0 || angles[1] != 0 || angles[2] != 0;
        float r = 0;
        if (rot) for (int j = 0; j < 3; j++) r = fmaxf(r, fmaxf(fabsf(bm->mins[j]), fabsf(bm->maxs[j])));
        r *= 1.74f;
        for (int k = 0; k < 3; k++) {
            pe->mins[k] = rot ? origin[k] - r : origin[k] + bm->mins[k];
            pe->maxs[k] = rot ? origin[k] + r : origin[k] + bm->maxs[k];
        }
        pe->iuser4 = 1;   // bounds valid
    }
}

void pm_build_physents(void)
{
    // Like the PC server (SV_AddLinksToPM), only hand pm_shared the brush entities near the player.
    static const float zero[3] = {0, 0, 0};
    const float range = 512.0f;
    pm->numphysent = 0;
    pm->nummoveent = 0;
    add_physent(pm->physents, &pm->numphysent, MAX_PHYSENTS, 0, &world.models[0], zero, zero, SOLID_BSP, 0, 0);
    for (int i = 1; i < num_entities; i++) {
        entity_t *e = &entities[i];
        if (e->model <= 0 || e->removed) continue;
        if (e->solid != SOLID_BSP && e->contents == 0) continue;
        bmodel_t *bm = &world.bmodels[e->model];
        bool rotated = e->angles[0] != 0 || e->angles[1] != 0 || e->angles[2] != 0;
        bool near = true;
        for (int k = 0; k < 3 && near; k++) {
            float mn, mx;
            if (rotated) {
                float r = 0;
                for (int j = 0; j < 3; j++) r = fmaxf(r, fmaxf(fabsf(bm->mins[j]), fabsf(bm->maxs[j])));
                r *= 1.74f;
                mn = e->origin[k] - r; mx = e->origin[k] + r;
            } else {
                mn = e->origin[k] + bm->mins[k]; mx = e->origin[k] + bm->maxs[k];
            }
            if (pm->origin[k] + range < mn || pm->origin[k] - range > mx) near = false;
        }
        if (!near) continue;
        if (e->solid == SOLID_BSP)
            add_physent(pm->physents, &pm->numphysent, MAX_PHYSENTS, i, &world.models[e->model], e->origin,
                        e->angles, SOLID_BSP, 0, e->rendermode);
        else
            add_physent(pm->moveents, &pm->nummoveent, MAX_MOVEENTS, i, &world.models[e->model], e->origin,
                        e->angles, SOLID_NOT, e->contents, e->rendermode);
    }
    // monsters: solid boxes (SOLID_SLIDEBOX in the PC game)
    npc_box_t boxes[24];
    int nb = npcs_get_boxes(boxes, 24, pm->origin, range);
    for (int b = 0; b < nb && pm->numphysent < MAX_PHYSENTS; b++) {
        physent_t *pe = &pm->physents[pm->numphysent++];
        memset(pe, 0, sizeof(*pe));
        pe->info = -1;
        pe->solid = SOLID_BBOX;
        VectorCopy(boxes[b].origin, pe->origin);
        VectorCopy(boxes[b].mins, pe->mins);
        VectorCopy(boxes[b].maxs, pe->maxs);
        strcpy(pe->name, "monster");
    }
    pm->numvisent = 0;
}

int pm_entity_for_physent(int pe)
{
    if (pe < 0 || pe >= pm->numphysent) return -1;
    return pm->physents[pe].info;
}

// does the player box at `pos` intersect brush entity `ent`?
bool pm_player_in_entity(int ent, const float *pos)
{
    entity_t *e = &entities[ent];
    if (e->model <= 0) return false;
    physent_t pe;
    memset(&pe, 0, sizeof(pe));
    pe.model = &world.models[e->model];
    pe.solid = SOLID_BSP;
    VectorCopy(e->origin, pe.origin);
    VectorCopy(e->angles, pe.angles);
    float offset[3], test[3];
    hull_t *hull = hull_for_physent(&pe, pm->usehull, offset);
    VectorSubtract(pos, offset, test);
    if (has_angles(&pe)) {
        float f[3], r[3], u[3], t[3];
        AngleVectors(pe.angles, f, r, u);
        VectorCopy(test, t);
        test[0] = DotProduct(t, f); test[1] = -DotProduct(t, r); test[2] = DotProduct(t, u);
    }
    return bsp_hull_point_contents(hull, hull->firstclipnode, test) == CONTENTS_SOLID;
}

bool pm_position_free(const float *pos)
{
    return test_position_ex((float *)pos, NULL, NULL) < 0;
}

void pm_run(player_input_t *in, float dt)
{
    pm_build_physents();
    usercmd_t *cmd = &pm->cmd;
    memset(cmd, 0, sizeof(*cmd));
    int msec = (int)(dt * 1000.0f + 0.5f);
    if (msec < 1) msec = 1;
    if (msec > 100) msec = 100;
    cmd->msec = msec;
    cmd->forwardmove = in->forward;
    cmd->sidemove = in->side;
    cmd->upmove = in->up;
    cmd->buttons = in->buttons;
    VectorCopy(in->viewangles, cmd->viewangles);
    VectorCopy(in->viewangles, pm->angles);
    // The PC engine refreshes these from sv_maxspeed every frame; pm_shared scales maxspeed
    // down while +use is held, so without this reset holding "use" would freeze the player.
    pm->maxspeed = movevars.maxspeed;
    pm->clientmaxspeed = movevars.maxspeed;
    pm->frametime = msec / 1000.0f;
    pm->time += msec;
    pm->player_index = 0;
    pm->movetype = in->noclip ? MOVETYPE_NOCLIP : (pm->movetype == MOVETYPE_NOCLIP ? MOVETYPE_WALK : pm->movetype);
    PM_Move(pm, 1);
    pm->oldbuttons = cmd->buttons;
}

// ---------------------------------------------------------------- accessors for the rest of the game
void pm_get_origin(float *o) { VectorCopy(pm->origin, o); }
void pm_set_origin(const float *o) { VectorCopy(o, pm->origin); }
void pm_get_angles(float *a) { VectorCopy(pm->angles, a); }
void pm_get_velocity(float *v) { VectorCopy(pm->velocity, v); }
void pm_set_velocity(const float *v) { VectorCopy(v, pm->velocity); }
void pm_get_view_ofs(float *v) { VectorCopy(pm->view_ofs, v); }
void pm_get_punch(float *v) { VectorCopy(pm->punchangle, v); }
int  pm_get_groundent(void) { return pm->onground; }
int  pm_get_waterlevel(void) { return pm->waterlevel; }
void pm_get_hull(float *mins, float *maxs)
{
    VectorCopy(pm->player_mins[pm->usehull], mins);
    VectorCopy(pm->player_maxs[pm->usehull], maxs);
}
void pm_add_basevelocity(const float *v) { VectorAdd(pm->basevelocity, v, pm->basevelocity); }
void pm_reset_player(const float *origin)
{
    VectorCopy(origin, pm->origin);
    VectorClear(pm->velocity);
    VectorClear(pm->basevelocity);
    VectorClear(pm->punchangle);
    VectorSet(pm->view_ofs, 0, 0, 28);
    pm->flags = 0;
    pm->usehull = 0;
    pm->bInDuck = 0;
    pm->flDuckTime = 0;
    pm->onground = -1;
    pm->waterlevel = 0;
    pm->movetype = MOVETYPE_WALK;
}
void pm_clear_basevelocity(void) { VectorClear(pm->basevelocity); }

// Find a nearby position where the standing player hull fits (used when a map is started
// from the menu: some info_player_starts are only valid for level transitions).
bool pm_find_free_position_hull(float *origin, int usehull);
bool pm_find_free_position(float *origin) { return pm_find_free_position_hull(origin, 0); }
// arrive crouched (a level transition inside a vent: t0a0 -> t0a0a)
void pm_set_ducked(void)
{
    pm->usehull = 1;
    pm->flags |= FL_DUCKING;
    pm->bInDuck = 0;
    pm->view_ofs[2] = 12;     // VEC_DUCK_VIEW
}
bool pm_find_free_position_hull(float *origin, int usehull)
{
    pm_build_physents();
    int old = pm->usehull;
    pm->usehull = usehull;
    bool ok = false;
    if (pm_position_free(origin)) ok = true;
    for (int r = 0; r <= 512 && !ok; r += (r < 96 ? 8 : 32)) {
        for (int dz = 0; dz <= 72 && !ok; dz += (r < 96 ? 4 : 18)) {
            for (int a = 0; a < 8 && !ok; a++) {
                if (r == 0 && a > 0) break;
                float t[3] = {origin[0] + r * cosf(a * M_PI / 4), origin[1] + r * sinf(a * M_PI / 4), origin[2] + dz};
                if (pm_position_free(t)) { VectorCopy(t, origin); ok = true; }
            }
        }
    }
    pm->usehull = old;
    return ok;
}

// Watchdog: if the player has been embedded in solid geometry (a bad level-transition offset,
// a door closing on them) for a moment, move them to the nearest free spot.
static float stuck_time;
bool pm_unstick(float dt)
{
    if (pm->movetype == MOVETYPE_NOCLIP) { stuck_time = 0; return false; }
    pm_build_physents();
    if (test_position_ex(pm->origin, NULL, NULL) < 0) { stuck_time = 0; return false; }
    stuck_time += dt;
    if (stuck_time < 0.5f) return false;
    stuck_time = 0;
    float o[3];
    VectorCopy(pm->origin, o);
    if (pm_find_free_position(o)) {
        VectorCopy(o, pm->origin);
        VectorClear(pm->velocity);
        debugf("unstuck player -> %.0f %.0f %.0f\n", o[0], o[1], o[2]);
        return true;
    }
    return false;
}

// driving a func_tracktrain (CBasePlayer::PreThink sets FL_ONTRAIN: no walking, jumping or ducking)
void pm_set_ontrain(bool on)
{
    if (on) pm->flags |= FL_ONTRAIN;
    else pm->flags &= ~FL_ONTRAIN;
}

// ---------------------------------------------------------------- general traces for AI and weapons
// (SV_Move: world + solid brush entities, then monster boxes and the player)
static int hull_for_size(const float *mins, const float *maxs)
{
    if (!mins) return 0;
    float sx = maxs[0] - mins[0], sz = maxs[2] - mins[2];
    if (sx <= 8) return 0;
    if (sx <= 36) return sz <= 36 ? 3 : 1;
    return 2;
}

static void trace_brush(model_t *m, int hullnum, const float *origin, const float *angles, const float *start,
                        const float *end, btrace_t *tr)
{
    hull_t *hull = &m->hulls[hullnum];
    float s[3], e[3];
    VectorSubtract(start, origin, s);
    VectorSubtract(end, origin, e);
    bool rotated = angles && (angles[0] != 0 || angles[1] != 0 || angles[2] != 0);
    if (rotated) {
        float f[3], r[3], u[3], t[3];
        AngleVectors((float *)angles, f, r, u);
        VectorCopy(s, t); s[0] = DotProduct(t, f); s[1] = -DotProduct(t, r); s[2] = DotProduct(t, u);
        VectorCopy(e, t); e[0] = DotProduct(t, f); e[1] = -DotProduct(t, r); e[2] = DotProduct(t, u);
    }
    bsp_trace_hull(hull, s, e, tr);
    if (rotated && tr->fraction != 1.0f) {
        float n[3], ft[3], rt[3], ut[3];
        VectorCopy(tr->normal, n);
        AngleVectorsTranspose((float *)angles, ft, rt, ut);
        tr->normal[0] = DotProduct(n, ft);
        tr->normal[1] = DotProduct(n, rt);
        tr->normal[2] = DotProduct(n, ut);
    }
    for (int i = 0; i < 3; i++) tr->endpos[i] = start[i] + tr->fraction * (end[i] - start[i]);
}

static void merge(gtrace_t *out, btrace_t *tr, int ent, int npc, bool player)
{
    if (tr->allsolid) tr->startsolid = true;
    if (tr->startsolid) tr->fraction = 0;
    if (tr->fraction < out->fraction || (tr->startsolid && !out->startsolid)) {
        out->fraction = tr->fraction;
        out->startsolid = tr->startsolid;
        out->allsolid = tr->allsolid;
        VectorCopy(tr->normal, out->normal);
        out->ent = ent;
        out->npc = npc;
        out->player = player;
    }
}

// the solid brush entities and their world bounds, gathered once a frame (SV_LinkEdict's areanodes, simply)
typedef struct { int ent; float mn[3], mx[3]; } solid_t;
static solid_t solids[256];
static int num_solid;
static bool solid_valid;

void game_trace_begin_frame(void)
{
    num_solid = 0;
    solid_valid = true;
    for (int i = 1; i < num_entities && num_solid < 256; i++) {
        entity_t *en = &entities[i];
        if (en->model <= 0 || en->removed || en->solid != SOLID_BSP) continue;
        bmodel_t *bm = &world.bmodels[en->model];
        solid_t *sd = &solids[num_solid++];
        sd->ent = i;
        bool rotated = en->angles[0] != 0 || en->angles[1] != 0 || en->angles[2] != 0;
        for (int k = 0; k < 3; k++) {
            if (rotated || en->type == ENT_DOOR_ROTATING || en->type == ENT_ROTATING || en->type == ENT_ROT_BUTTON) {
                float r = 0;
                for (int j = 0; j < 3; j++) r = fmaxf(r, fmaxf(fabsf(bm->mins[j]), fabsf(bm->maxs[j])));
                r *= 1.74f;
                sd->mn[k] = en->origin[k] - r; sd->mx[k] = en->origin[k] + r;
            } else {
                // movers move during the frame: pad by what they can travel
                float pad = en->active ? 64 : 1;
                sd->mn[k] = en->origin[k] + bm->mins[k] - pad; sd->mx[k] = en->origin[k] + bm->maxs[k] + pad;
            }
        }
    }
}

void game_trace(const float *start, const float *end, const float *mins, const float *maxs, int flags, int ignore_npc,
                gtrace_t *out)
{
    static const float zero[3] = {0, 0, 0};
    int hn = hull_for_size(mins, maxs);
    // the BSP hull is centred on its own clip box: shift the trace so the object's box lines up
    float off[3] = {0, 0, 0};
    if (mins) for (int k = 0; k < 3; k++) off[k] = mins[k] - world.models[0].hulls[hn].clip_mins[k];
    float s[3], e[3];
    VectorAdd(start, off, s);
    VectorAdd(end, off, e);
    memset(out, 0, sizeof(*out));
    out->fraction = 1.0f;
    out->ent = -1;
    out->npc = -1;
    btrace_t tr;
    trace_brush(&world.models[0], hn, zero, NULL, s, e, &tr);
    merge(out, &tr, 0, -1, false);
    // segment bounds for quick rejection
    float bmin[3], bmax[3];
    for (int k = 0; k < 3; k++) {
        bmin[k] = fminf(start[k], end[k]) + (mins ? mins[k] : 0) - 1;
        bmax[k] = fmaxf(start[k], end[k]) + (maxs ? maxs[k] : 0) + 1;
    }
    if (!(flags & TR_WORLD_ONLY)) {
        if (!solid_valid) game_trace_begin_frame();
        for (int si = 0; si < num_solid && out->fraction > 0; si++) {
            solid_t *sd = &solids[si];
            int i = sd->ent;
            entity_t *en = &entities[i];
            if (en->removed || en->solid != SOLID_BSP) continue;
            if ((flags & TR_IGNORE_GLASS) && en->rendermode != 0) continue;
            if (bmax[0] < sd->mn[0] || bmin[0] > sd->mx[0] || bmax[1] < sd->mn[1] || bmin[1] > sd->mx[1] ||
                bmax[2] < sd->mn[2] || bmin[2] > sd->mx[2]) continue;
            trace_brush(&world.models[en->model], hn, en->origin, en->angles, s, e, &tr);
            merge(out, &tr, i, -1, false);
        }
    }
    float mn0[3] = {0, 0, 0}, mx0[3] = {0, 0, 0};
    if (mins) { VectorCopy(mins, mn0); VectorCopy(maxs, mx0); }
    if (!(flags & (TR_IGNORE_MONSTERS | TR_WORLD_ONLY))) {
        for (int i = 0; i < num_npcs; i++) {
            npc_t *n = &npcs[i];
            if (i == ignore_npc || !n->solid || entities[n->ent].removed) continue;
            bool hit = true;
            for (int k = 0; k < 3 && hit; k++)
                if (bmax[k] < n->origin[k] + n->mins[k] || bmin[k] > n->origin[k] + n->maxs[k]) hit = false;
            if (!hit) continue;
            float bmn[3], bmx[3], ls[3], le[3];
            for (int k = 0; k < 3; k++) {
                bmn[k] = n->mins[k] - mx0[k]; bmx[k] = n->maxs[k] - mn0[k];
                ls[k] = start[k] - n->origin[k]; le[k] = end[k] - n->origin[k];
            }
            hull_t *h = hull_for_box(bmn, bmx);
            bsp_trace_hull(h, ls, le, &tr);
            merge(out, &tr, n->ent, i, false);
        }
    }
    if (!(flags & (TR_IGNORE_PLAYER | TR_WORLD_ONLY)) && pm->movetype != MOVETYPE_NOCLIP) {
        float pmn[3], pmx[3], ls[3], le[3];
        for (int k = 0; k < 3; k++) {
            pmn[k] = pm->player_mins[pm->usehull][k] - mx0[k];
            pmx[k] = pm->player_maxs[pm->usehull][k] - mn0[k];
            ls[k] = start[k] - pm->origin[k]; le[k] = end[k] - pm->origin[k];
        }
        hull_t *h = hull_for_box(pmn, pmx);
        bsp_trace_hull(h, ls, le, &tr);
        merge(out, &tr, 0, -1, true);
        if (out->player) out->ent = 0;
    }
    for (int k = 0; k < 3; k++) out->endpos[k] = start[k] + out->fraction * (end[k] - start[k]);
}

bool game_visible(const float *a, const float *b)
{
    gtrace_t tr;
    game_trace(a, b, NULL, NULL, TR_IGNORE_MONSTERS | TR_IGNORE_PLAYER | TR_IGNORE_GLASS, -1, &tr);
    return tr.fraction >= 1.0f && !tr.startsolid;
}

int game_point_contents(const float *p) { return bsp_point_contents(p); }

float pm_get_fall_velocity(void) { return pm->flFallVelocity; }
int pm_get_flags(void) { return pm->flags; }
void pm_set_punch(const float *v) { VectorCopy(v, pm->punchangle); }
void pm_set_maxspeed_scale(float s) { (void)s; }
bool pm_ducking(void) { return pm->usehull == 1; }

bool pm_noclip_active(void) { return pm->movetype == MOVETYPE_NOCLIP; }
int  pm_get_oldwaterlevel(void) { return pm->waterlevel; }
void pm_set_view_ofs_z(float z) { pm->view_ofs[2] = z; }
