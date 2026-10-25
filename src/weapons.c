// HL64 - the player's weapons (dlls/weapons.cpp, glock.cpp, python.cpp, mp5.cpp, shotgun.cpp,
// crossbow.cpp, rpg.cpp, gauss.cpp, egon.cpp, hornetgun.cpp, handgrenade.cpp, satchel.cpp,
// tripmine.cpp, squeakgrenade.cpp, crowbar.cpp), their projectiles, the first-person view
// model, the ammo / weapon-selection HUD, and simple 2D effects for beams and tracers.
#include <libdragon.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <t3d/t3d.h>
#include "bsp.h"
#include "game.h"
#include "studio.h"
#include "npc.h"
#include "font.h"
#include "weapons.h"

void pm_get_origin(float *o);
void pm_get_velocity(float *v);
void pm_set_velocity(const float *v);
void pm_get_punch(float *v);
void pm_set_punch(const float *v);
int  pm_get_waterlevel(void);
T3DViewport *render_viewport(void);

enum { AM_9MM, AM_357, AM_BUCK, AM_BOLT, AM_ROCKET, AM_URANIUM, AM_HORNET, AM_ARGREN, AM_HGREN, AM_SATCHEL, AM_TRIP, AM_SNARK, AM_COUNT };
static const int ammo_max[AM_COUNT] = {250, 36, 125, 50, 5, 100, 8, 10, 10, 5, 5, 15};

typedef struct {
    int id;
    const char *cls, *vmodel;
    int slot, pos;
    int ammo1, ammo2, maxclip, give;
    // 320hud1 icon (weapon, weapon_s) rects and the ammo icon on 320hud2
    int wx, wy, ax, ay;
    const char *s_idle, *s_draw, *s_holster, *s_fire, *s_fire2, *s_reload;
    const char *snd;
    int body;
    int cx, cy;      // crosshair in crosshairs.spr
} wdef_t;

// slots/positions and HUD icons from the game's sprites/weapon_*.txt (320 entries)
static const wdef_t W[] = {
    {0},
    {WEAPON_CROWBAR, "weapon_crowbar", "v_crowbar", 0, 0, -1, -1, -1, 0, 0, 0, -1, -1, "idle1", "draw", "holster", "attack1hit", "attack2hit", NULL, NULL, 0, 24, 0},
    {WEAPON_GLOCK, "weapon_9mmhandgun", "v_9mmhandgun", 1, 0, AM_9MM, -1, 17, 17, 0, 40, 0, 16, "idle1", "draw", "holster", "shoot", "shoot", "reload", "weapons/pl_gun3.wav", 0, 24, 0},
    {WEAPON_PYTHON, "weapon_357", "v_357", 1, 1, AM_357, -1, 6, 6, 0, 80, 18, 16, "idle1", "draw", "holster", "fire1", NULL, "reload", "weapons/357_shot1.wav", 0, 48, 0},
    {WEAPON_MP5, "weapon_9mmAR", "v_9mmar", 2, 0, AM_9MM, AM_ARGREN, 50, 25, 0, 120, 0, 16, "idle1", "deploy", NULL, "shoot", "grenade", "reload", "weapons/hks1.wav", 0, 0, 48},
    {0},
    {WEAPON_CROSSBOW, "weapon_crossbow", "v_crossbow", 2, 2, AM_BOLT, -1, 5, 5, 80, 0, 72, 16, "idle1", "draw1", "holster1", "fire1", NULL, "reload", "weapons/xbow_fire1.wav", 0, 72, 0},
    {WEAPON_SHOTGUN, "weapon_shotgun", "v_shotgun", 2, 1, AM_BUCK, -1, 8, 12, 0, 160, 54, 16, "sm_idle", "draw", "reholster", "shoot", "shoot_big", "reload", "weapons/sbarrel1.wav", 0, 48, 24},
    {WEAPON_RPG, "weapon_rpg", "v_rpg", 3, 0, AM_ROCKET, -1, 1, 1, 80, 40, 90, 16, "idle", "draw1", "holster1", "fire", NULL, "reload", "weapons/rocketfire1.wav", 0, 24, 48},
    {WEAPON_GAUSS, "weapon_gauss", "v_gauss", 3, 1, AM_URANIUM, -1, -1, 20, 80, 80, 0, 34, "idle", "draw", "holster", "fire2", "spin", NULL, "weapons/gauss2.wav", 0, 48, 48},
    {WEAPON_EGON, "weapon_egon", "v_egon", 3, 2, AM_URANIUM, -1, -1, 20, 80, 120, 0, 34, "idle1", "draw", "holster", "fire1", NULL, NULL, NULL, 0, 72, 48},
    {WEAPON_HORNETGUN, "weapon_hornetgun", "v_hgun", 3, 3, AM_HORNET, -1, -1, 8, 80, 160, 16, 36, "idle1", "up", "down", "Shoot", "Shoot", NULL, NULL, 0, 72, 24},
    {WEAPON_HANDGRENADE, "weapon_handgrenade", "v_grenade", 4, 0, AM_HGREN, -1, -1, 5, 160, 0, 36, 34, "idle", "draw", "holster", "throw1", NULL, NULL, NULL, 0, 0, 0},
    {WEAPON_TRIPMINE, "weapon_tripmine", "v_tripmine", 4, 2, AM_TRIP, -1, -1, 1, 160, 80, 72, 34, "idle1", "draw", "holster", "place", NULL, NULL, NULL, 0, 0, 0},
    {WEAPON_SATCHEL, "weapon_satchel", "v_satchel", 4, 1, AM_SATCHEL, -1, -1, 1, 160, 40, 54, 34, "idle1", "draw", NULL, "drop", NULL, NULL, NULL, 0, 0, 0},
    {WEAPON_SNARK, "weapon_snark", "v_squeak", 4, 3, AM_SNARK, -1, -1, 5, 160, 120, 90, 34, "idle1", "up", "down", "throw", NULL, NULL, NULL, 0, 0, 0},
};
#define NUM_W ((int)(sizeof(W) / sizeof(W[0])))

#ifndef NOPICKSND
#define NOPICKSND 0
#endif
static const wdef_t *wdef(int id) { return (id > 0 && id < NUM_W && W[id].id == id) ? &W[id] : NULL; }

// ------------------------------------------------------------------ state
static int cur = 0;              // current weapon id (0 = none / holstered)
static int pending = 0;          // switching to
static float next_attack, next_attack2, idle_time, reload_end, holster_end;
static bool reloading;
static studio_t *vmodel;
static int vseq;
static float vframe;
static bool vseq_done;
static int vbody;
static float wtime;              // weapon clock
static bool prev_fire, prev_fire2;
// selection HUD
static float select_show;
static int select_slot = -1;
// special weapon states
static int shotgun_reload_state;     // 0 none, 1 starting, 2 shells going in
static float gauss_charge_start;
static bool gauss_charging;
static int egon_state;              // 0 off, 1 firing
static float egon_ammo_time;
static float grenade_start;          // pin pulled (0 = not)
static float hornet_regen;
static int satchel_out;              // satchels in the world
static bool rpg_laser = true;
static int muzzle_flash;
static float bob_t;

// ------------------------------------------------------------------ effects (2D-projected lines)
typedef struct { float a[3], b[3]; uint8_t r, g, bl, alpha; float life, end; float width; uint8_t tex; bool world_w; } beam_t;
static int beam_tex_next;   // 1 = lgtning.spr, 2 = laserbeam.spr, 5 = xbeam1.spr
static bool beam_world_next; // width in world units (perspective) instead of pixels
static sprite_t *fx_spr[5];  // lgtning, laserbeam, wallpuff, blood, xbeam1
#define MAX_BEAMS 24
static beam_t beams[MAX_BEAMS];

void game_effect_lightning(const float *a, const float *b, int r, int g, int bl);
void game_effect_particles(const float *o, int count, float speed, int r, int g, int b, float life, float grav);
static void add_beam(const float *a, const float *b, int r, int g, int bl, float life, float width)
{
    int best = 0;
    for (int i = 0; i < MAX_BEAMS; i++) { if (beams[i].end < wtime) { best = i; break; } if (beams[i].end < beams[best].end) best = i; }
    beam_t *bm = &beams[best];
    memcpy(bm->a, a, 12);
    memcpy(bm->b, b, 12);
    bm->r = r; bm->g = g; bm->bl = bl; bm->alpha = 255;
    bm->life = life;
    bm->end = wtime + life;
    bm->width = width;
    bm->tex = beam_tex_next;
    bm->world_w = beam_world_next;
}

void game_effect_tracer(const float *from, const float *to)
{
    if (rand() % 3) return;   // tracers on some bullets (iTracerFreq)
    add_beam(from, to, 255, 220, 120, 0.06f, 1);
}
void game_effect_beam(const float *from, const float *to, int r, int g, int b) { game_effect_lightning(from, to, r, g, b); game_effect_particles(to, 6, 80, r, g, b, 0.3f, 0); }
void game_effect_beam_plain(const float *a, const float *b, int r, int g, int bl) { beam_tex_next = 2; add_beam(a, b, r, g, bl, 0.12f, 6); beam_tex_next = 0; }
void game_effect_debris(const float *origin, int material) { (void)origin; (void)material; }

static void add_beam(const float *a, const float *b, int r, int g, int bl, float life, float width);
// particles: sparks, electricity crackle, impact puffs (2D-projected points)
typedef struct { float o[3], v[3]; float end, life; uint8_t r, g, b, kind; float grav; } part_t;   // kind: 0 dot, 1 wallpuff, 2 blood
#define MAX_PARTS 96
static part_t parts[MAX_PARTS];
static int part_next;
void game_effect_particles(const float *o, int count, float speed, int r, int g, int b, float life, float grav)
{
    for (int i = 0; i < count; i++) {
        part_t *p = &parts[part_next++ % MAX_PARTS];
        memcpy(p->o, o, 12);
        for (int k = 0; k < 3; k++) p->v[k] = ((rand() % 200) - 100) / 100.0f * speed;
        p->v[2] += speed * 0.3f;
        p->life = life * (0.5f + (rand() % 100) / 200.0f);
        p->end = wtime + p->life;
        p->r = r; p->g = g; p->b = b; p->grav = grav; p->kind = 0;
    }
}
// textured impact sprite (wallpuff.spr / blood.spr)
static void effect_sprite(const float *o, int kind, int r, int g, int b, float life)
{
    part_t *p = &parts[part_next++ % MAX_PARTS];
    memcpy(p->o, o, 12);
    p->v[0] = p->v[1] = 0; p->v[2] = kind == 1 ? 20 : -10;
    p->life = life; p->end = wtime + life;
    p->r = r; p->g = g; p->b = b; p->grav = 0; p->kind = kind;
}
void game_effect_blood(const float *o) { effect_sprite(o, 2, 200, 20, 10, 0.6f); }
// blood when something is shot (UTIL_BloodStream + UTIL_BloodDrips): blood.spr splat and a spray of drops
void game_effect_blood_color(const float *o, bool yellow)
{
    if (yellow) { effect_sprite(o, 2, 210, 190, 20, 0.6f); game_effect_particles(o, 6, 90, 200, 180, 20, 0.5f, 400); }
    else { effect_sprite(o, 2, 200, 20, 10, 0.6f); game_effect_particles(o, 6, 90, 150, 10, 10, 0.5f, 400); }
}

// ---- gibs (CGib::SpawnRandomGibs with models/hgibs.mdl / agibs.mdl): bouncing body parts
typedef struct { studio_t *m; float o[3], v[3], yaw, spin; int body; float end; bool rest; } gib_t;
#define MAX_GIBS 32
static gib_t gibs[MAX_GIBS];
static int gib_next;
void game_spawn_gibs(const float *o, int count, bool alien, float speed)
{
    studio_t *m = studio_get(alien ? "agibs" : "hgibs");
    if (!m) { game_effect_particles(o, 20, 200, 140, 10, 10, 1.2f, 600); return; }
    int nbody = alien ? 4 : 11;
    for (int i = 0; i < count; i++) {
        gib_t *g = &gibs[gib_next++ % MAX_GIBS];
        g->m = m;
        for (int k = 0; k < 3; k++) g->o[k] = o[k] + ((rand() % 32) - 16);
        for (int k = 0; k < 3; k++) g->v[k] = ((rand() % 200) - 100) / 100.0f * speed;
        g->v[2] += speed * 0.8f + (rand() % 100);
        g->yaw = rand() % 360;
        g->spin = (rand() % 600) - 300;
        g->body = !alien && i == 0 ? 0 : (alien ? rand() % nbody : 1 + rand() % (nbody - 1));   // one skull, then parts
        g->end = wtime + 8 + (rand() % 400) / 100.0f;
        g->rest = false;
    }
    game_effect_blood_color(o, alien);
}
static void gibs_think(float dt)
{
    for (int i = 0; i < MAX_GIBS; i++) {
        gib_t *g = &gibs[i];
        if (!g->m || g->end < wtime || g->rest) continue;
        g->v[2] -= 800 * dt;
        float to[3];
        for (int k = 0; k < 3; k++) to[k] = g->o[k] + g->v[k] * dt;
        gtrace_t tr;
        game_trace(g->o, to, NULL, NULL, TR_IGNORE_MONSTERS | TR_IGNORE_PLAYER, -1, &tr);
        if (tr.fraction < 1) {
            // bounce off with a loss (CGib::BounceGibTouch), leave a blood splat now and then
            float vn = g->v[0] * tr.normal[0] + g->v[1] * tr.normal[1] + g->v[2] * tr.normal[2];
            for (int k = 0; k < 3; k++) g->v[k] = (g->v[k] - 1.6f * vn * tr.normal[k]) * 0.55f;
            memcpy(g->o, tr.endpos, 12);
            for (int k = 0; k < 3; k++) g->o[k] += tr.normal[k] * 0.5f;
            g->spin *= 0.5f;
            if (tr.normal[2] > 0.7f && g->v[0] * g->v[0] + g->v[1] * g->v[1] + g->v[2] * g->v[2] < 900) g->rest = true;
        } else memcpy(g->o, to, 12);
        g->yaw += g->spin * dt;
    }
}

void game_effect_sparks(const float *o) { game_effect_particles(o, 10, 140, 255, 220, 120, 0.6f, 500); }
// a jagged electric arc between two points (env_beam / zaps)
void game_effect_lightning(const float *a, const float *b, int r, int g, int bl)
{
    if (fx_spr[0]) { beam_tex_next = 1; add_beam(a, b, r, g, bl, 0.12f, 10); beam_tex_next = 0; return; }
    float prev[3];
    memcpy(prev, a, 12);
    for (int s = 1; s <= 5; s++) {
        float t = s / 5.0f, p[3];
        for (int k = 0; k < 3; k++) p[k] = a[k] + (b[k] - a[k]) * t + (s < 5 ? ((rand() % 200) - 100) / 100.0f * 10 : 0);
        add_beam(prev, p, r, g, bl, 0.12f, 2);
        memcpy(prev, p, 12);
    }
}

// textured billboards: zerogxplode.spr explosions (animated), muzzleflash1-3.spr, gluon flare
static sprite_t *fx_expl[8], *fx_muzzle[3], *fx_xspark[11];
// kind 0 explosion, 1..3 muzzle, 4 gluon flare (xspark1.spr); view = the first-person gun's muzzle
typedef struct { int kind; float o[3], rad, start, life; uint8_t r, g, b; bool view; } bill_t;
#define MAX_BILLS 16
static bill_t bills[MAX_BILLS];
static int bill_next;
// TE_BEAMCYLINDER (the houndeye's sonic blast): a ring of wall 'height' units tall around o that
// grows to maxr over its life and fades
typedef struct { float o[3], maxr, height, start, life; uint8_t r, g, b; } ring_t;
#define MAX_RINGS 8
static ring_t rings[MAX_RINGS];
void game_effect_shockring(const float *o, float maxr, float height, float life, int r, int g, int b)
{
    int best = 0;
    for (int i = 0; i < MAX_RINGS; i++) { if (rings[i].start + rings[i].life < wtime) { best = i; break; } if (rings[i].start < rings[best].start) best = i; }
    ring_t *rg = &rings[best];
    memcpy(rg->o, o, 12);
    rg->maxr = maxr; rg->height = height; rg->start = wtime; rg->life = life;
    rg->r = r; rg->g = g; rg->b = b;
}
static void add_bill(int kind, const float *o, float rad, float life, int r, int g, int b)
{
    bill_t *bl = &bills[bill_next++ % MAX_BILLS];
    bl->kind = kind; memcpy(bl->o, o, 12); bl->rad = rad; bl->start = wtime; bl->life = life;
    bl->r = r; bl->g = g; bl->b = b;
    bl->view = false;
}
// the view model's muzzle (attachment 0), updated every frame it is drawn
static float vm_att[1][3];
static bool vm_att_ok;
static void add_view_flash(float rad)
{
    add_bill(1 + rand() % 3, vm_att[0], rad, 0.06f, 255, 255, 255);
    bills[(bill_next - 1) % MAX_BILLS].view = true;
}
void game_effect_flash(const float *o, float rad) { add_bill(4, o, rad, 0.5f, 255, 255, 255); }
void game_effect_muzzleflash(const float *o, float rad) { add_bill(1 + rand() % 3, o, rad, 0.06f, 255, 255, 255); }
typedef struct { float o[3]; float end, size; } flash_t;
static flash_t flashes[8];
void game_effect_explosion(const float *origin, float magnitude)
{
    {   // CGrenade::Explode: zerogxplode.spr scaled by the damage, plus a puff of sparks
        float rad = magnitude > 0 ? magnitude * 0.9f : 90;
        if (rad < 40) rad = 40;
        if (rad > 200) rad = 200;
        float o[3] = {origin[0], origin[1], origin[2] + rad * 0.3f};
        add_bill(0, o, rad, 0.75f, 255, 255, 255);
        game_effect_particles(origin, 12, 260, 255, 200, 90, 0.8f, 500);
        return;
    }
    for (int i = 0; i < 8; i++) if (flashes[i].end < wtime) {
        memcpy(flashes[i].o, origin, 12);
        flashes[i].end = wtime + 0.35f;
        flashes[i].size = magnitude > 0 ? magnitude : 100;
        break;
    }
}

// ------------------------------------------------------------------ radius damage (RadiusDamage)
void game_radius_damage(const float *origin, float damage, float radius, int attacker_npc, int dmgtype)
{
    if (radius <= 0) radius = damage * 2.5f;
    float pe[3];
    player_get_eye(pe);
    float po[3];
    pm_get_origin(po);
    float d = sqrtf((po[0] - origin[0]) * (po[0] - origin[0]) + (po[1] - origin[1]) * (po[1] - origin[1]) + (po[2] - origin[2]) * (po[2] - origin[2]));
    if (d < radius) {
        float o[3] = {origin[0], origin[1], origin[2] + 1};
        if (game_visible(o, pe) || game_visible(o, po)) player_take_damage(damage * (1 - d / radius), dmgtype, origin, attacker_npc >= 0 ? npcs[attacker_npc].ent : -1);
    }
    for (int i = 0; i < num_npcs; i++) {
        npc_t *n = &npcs[i];
        // corpses too: an explosion gibs the bodies around it (DeadTakeDamage, DMG_BLAST)
        extern bool npc_corpse_box(int i, float *mn, float *mx);
        float cmn[3], cmx[3];
        bool body = !npc_alive(i);
        if (body && !npc_corpse_box(i, cmn, cmx)) continue;
        float c[3] = {n->origin[0], n->origin[1], n->origin[2] + (body ? (cmn[2] + cmx[2]) * 0.5f : (n->mins[2] + n->maxs[2]) * 0.5f)};
        float dd = sqrtf((c[0] - origin[0]) * (c[0] - origin[0]) + (c[1] - origin[1]) * (c[1] - origin[1]) + (c[2] - origin[2]) * (c[2] - origin[2]));
        if (dd >= radius) continue;
        float o[3] = {origin[0], origin[1], origin[2] + 1};
        if (!game_visible(o, c)) continue;
        float dir[3] = {c[0] - origin[0], c[1] - origin[1], c[2] - origin[2]};
        npc_take_damage(i, damage * (1 - dd / radius), dmgtype, dir, attacker_npc < 0 ? 0 : attacker_npc + 1);
    }
    // breakables in range
    for (int i = 1; i < num_entities; i++) {
        if (!ents_brush_is_breakable(i)) continue;
        entity_t *e = &entities[i];
        bmodel_t *bm = &world.bmodels[e->model];
        float c[3];
        for (int k = 0; k < 3; k++) c[k] = e->origin[k] + (bm->mins[k] + bm->maxs[k]) * 0.5f;
        float dd = sqrtf((c[0] - origin[0]) * (c[0] - origin[0]) + (c[1] - origin[1]) * (c[1] - origin[1]) + (c[2] - origin[2]) * (c[2] - origin[2]));
        if (dd < radius + 32) ents_damage_brush(i, damage * (1 - fminf(dd, radius) / (radius + 32)), dmgtype, NULL);
    }
    npc_hear(origin, 1500, -1);
}

static void explode(const float *o, float dmg, int owner)
{
    static const char *ex[] = {"weapons/explode3.wav", "weapons/explode4.wav", "weapons/explode5.wav"};
    snd_emit(-1, CHAN_AUTO, ex[rand() % 3], 1.0f, 0.3f, PITCH_NORM, o);
    game_effect_explosion(o, dmg);
    game_radius_damage(o, dmg, dmg * 2.5f, owner > 0 ? owner - 1 : -1, DMG_BLAST);
    float pe[3];
    pm_get_origin(pe);
    float d = sqrtf((pe[0] - o[0]) * (pe[0] - o[0]) + (pe[1] - o[1]) * (pe[1] - o[1]));
    if (d < 1000) game_shake(6 * (1 - d / 1000), 30, 0.8f);
}

// ------------------------------------------------------------------ projectiles
enum { P_NONE, P_HGRENADE, P_ARGRENADE, P_ROCKET, P_BOLT, P_HORNET_P, P_SATCHEL, P_TRIPMINE, P_SNARK, P_GAUSSDUMMY,
       P_SPIT = 10, P_HORNET = 11, P_BALL = 12, P_MORTAR = 13, P_TELEBALL = 14, P_NIHZAP = 15 };
typedef struct {
    int kind;
    float o[3], v[3], yaw, pitch;
    float die, armed;
    int owner;          // 0 player, n+1 npc
    int target;         // for homing
    bool stuck;
    float beam_end[3];
    studio_t *model;
    float frame;
} proj_t;
#define MAX_PROJ 32
static proj_t projs[MAX_PROJ];

int proj_spawn(int kind, const float *origin, const float *dir, int owner, int target)
{
    int s = -1;
    for (int i = 0; i < MAX_PROJ; i++) if (projs[i].kind == P_NONE) { s = i; break; }
    if (s < 0) return -1;
    proj_t *p = &projs[s];
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    memcpy(p->o, origin, 12);
    p->owner = owner;
    p->target = target;
    float speed = 0;
    const char *model = NULL;
    switch (kind) {
    case P_HGRENADE: speed = 500; p->die = wtime + 3.0f; model = "w_grenade"; break;
    case P_ARGRENADE: speed = 800; p->die = wtime + 10; model = "grenade"; break;
    case P_ROCKET: speed = 250; p->die = wtime + 15; model = "rpgrocket"; p->armed = wtime + 0.4f; break;
    case P_BOLT: speed = pm_get_waterlevel() == 3 ? 1000 : 2000; p->die = wtime + 10; model = "crossbow_bolt"; break;
    case P_SATCHEL: speed = 274; p->die = wtime + 600; model = "w_satchel"; break;
    case P_SNARK: speed = 200; p->die = wtime + 15; model = "w_squeak"; break;
    case P_HORNET: case P_HORNET_P: speed = owner == 0 ? 300 : 300; p->die = wtime + 5; model = "hornet"; break;
    case P_SPIT: speed = 900; p->die = wtime + 4; break;
    case P_BALL: speed = 500; p->die = wtime + 5; break;
    case P_MORTAR: speed = 1; p->die = wtime + 8; break;            // velocity set by the caller (a lob)
    case P_TELEBALL: speed = 300; p->die = wtime + 10; break;
    case P_NIHZAP: speed = 400; p->die = wtime + 6; break;
    case P_TRIPMINE: speed = 1; p->die = wtime + 1e6f; model = "v_tripmine"; p->armed = wtime + 2.5f; break;   // v = the beam direction
    }
    for (int k = 0; k < 3; k++) p->v[k] = dir[k] * speed;
    if ((kind == P_HGRENADE || kind == P_SATCHEL || kind == P_SNARK) && owner == 0) {
        float pv[3];
        pm_get_velocity(pv);
        for (int k = 0; k < 3; k++) p->v[k] += pv[k];
    }
    if (kind == P_SPIT) p->v[2] += 150;
    p->yaw = atan2f(dir[1], dir[0]) * 180 / (float)M_PI;
    p->pitch = 0;
    if (model) p->model = studio_get(model);
    return s;
}

// func_tank shots (func_tank.cpp: CFuncTankGun / Laser / Rocket / Mortar) from a barrel along dir
void game_tank_fire(int kind, const float *from, const float *dir, float spread, float dmg, bool by_player)
{
    float o[3] = {from[0] + dir[0] * 8, from[1] + dir[1] * 8, from[2] + dir[2] * 8};
    game_effect_muzzleflash(o, kind == 2 ? 14 : 9);
    if (kind == 2) {   // rocket
        proj_spawn(P_ROCKET, o, dir, by_player ? 0 : -1, -1);
        snd_emit(-1, CHAN_AUTO, "weapons/rocketfire1.wav", 1, ATTN_NORM, PITCH_NORM, o);
        return;
    }
    float r[3] = {dir[1], -dir[0], 0}, rl = sqrtf(r[0] * r[0] + r[1] * r[1]);
    if (rl < 0.01f) { r[0] = 1; r[1] = 0; rl = 1; }
    r[0] /= rl; r[1] /= rl;
    float u[3] = {r[1] * dir[2] - r[2] * dir[1], r[2] * dir[0] - r[0] * dir[2], r[0] * dir[1] - r[1] * dir[0]};
    float x = ((rand() % 1000) / 1000.0f - 0.5f) * spread, y = ((rand() % 1000) / 1000.0f - 0.5f) * spread;
    float d[3];
    for (int k = 0; k < 3; k++) d[k] = dir[k] + x * r[k] + y * u[k];
    float end[3] = {o[0] + d[0] * 4096, o[1] + d[1] * 4096, o[2] + d[2] * 4096};
    gtrace_t tr;
    game_trace(o, end, NULL, NULL, by_player ? TR_IGNORE_PLAYER : 0, -1, &tr);
    if (kind == 1) { beam_tex_next = 2; add_beam(o, tr.endpos, 255, 60, 60, 0.15f, 5); beam_tex_next = 0; snd_emit(-1, CHAN_AUTO, "weapons/electro4.wav", 1, ATTN_NORM, PITCH_NORM, o); }
    else if (kind == 3) { explode(tr.endpos, dmg > 0 ? dmg : 200, by_player ? 0 : -1); return; }
    else { snd_emit(-1, CHAN_AUTO, "weapons/hks1.wav", 0.8f, ATTN_NORM, PITCH_NORM, o); if (rand() % 2 == 0) game_effect_tracer(o, tr.endpos); }
    if (tr.fraction >= 1) return;
    if (tr.player) { if (!by_player) player_take_damage(dmg, kind == 1 ? DMG_ENERGYBEAM : DMG_BULLET, o, -1); }
    else if (tr.npc >= 0) { extern void npc_set_hitpoint(const float *p); npc_set_hitpoint(tr.endpos); npc_take_damage(tr.npc, dmg, kind == 1 ? DMG_ENERGYBEAM : DMG_BULLET, d, by_player ? 0 : -1); }
    else { if (tr.ent > 0) ents_damage_brush(tr.ent, dmg, DMG_BULLET, d); game_effect_particles(tr.endpos, 2, 70, 255, 220, 140, 0.3f, 300); }
}

// a projectile with a given velocity (lobs: grenades, Big Momma's mortar)
int proj_spawn_vel(int kind, const float *origin, const float *vel, int owner, int target)
{
    float dir[3] = {1, 0, 0};
    int s = proj_spawn(kind, origin, dir, owner, target);
    if (s >= 0) {
        memcpy(projs[s].v, vel, 12);
        projs[s].yaw = atan2f(vel[1], vel[0]) * 180 / (float)M_PI;
        if (kind == P_HGRENADE) projs[s].die = wtime + 3.5f;
    }
    return s;
}

// a hostile snark from a snark nest (monstermaker "monster_snark"): hunts the player
int proj_spawn_snark(const float *origin, float yaw)
{
    float a = yaw * (float)M_PI / 180, dir[3] = {cosf(a), sinf(a), 0};
    return proj_spawn(P_SNARK, origin, dir, -1, 0);
}

static void proj_hit_damage(proj_t *p, gtrace_t *tr, float dmg, int type)
{
    float dir[3] = {p->v[0], p->v[1], p->v[2]};
    if (tr->player) { if (p->owner != 0) player_take_damage(dmg, type, p->o, p->owner > 0 ? npcs[p->owner - 1].ent : -1); }
    else if (tr->npc >= 0) npc_take_damage(tr->npc, dmg, type, dir, p->owner);
    else if (tr->ent > 0) ents_damage_brush(tr->ent, dmg, type, dir);
}

static int nearest_target(const float *o, int owner, float range)
{
    int best = -2;
    float bd = range;
    // player-owned homing things go for monsters, monster-owned ones for the player
    if (owner != 0) {
        float po[3];
        pm_get_origin(po);
        float d = sqrtf((po[0] - o[0]) * (po[0] - o[0]) + (po[1] - o[1]) * (po[1] - o[1]) + (po[2] - o[2]) * (po[2] - o[2]));
        if (d < bd && player_alive()) { bd = d; best = 0; }
    }
    if (owner == 0) {
        for (int i = 0; i < num_npcs; i++) {
            npc_t *n = &npcs[i];
            if (!npc_alive(i) || (n->flags & 32 /* talker */)) continue;
            float d = sqrtf((n->origin[0] - o[0]) * (n->origin[0] - o[0]) + (n->origin[1] - o[1]) * (n->origin[1] - o[1]) + (n->origin[2] - o[2]) * (n->origin[2] - o[2]));
            if (d < bd) { bd = d; best = i + 1; }
        }
    }
    return best;
}

static void target_point(int t, float *o)
{
    if (t == 0) { player_get_eye(o); o[2] -= 12; }
    else { npc_t *n = &npcs[t - 1]; o[0] = n->origin[0]; o[1] = n->origin[1]; o[2] = n->origin[2] + (n->mins[2] + n->maxs[2]) * 0.5f; }
}

static void projs_think(float dt)
{
    for (int i = 0; i < MAX_PROJ; i++) {
        proj_t *p = &projs[i];
        if (p->kind == P_NONE) continue;
        p->frame += dt * 10;
        if (wtime > p->die) {
            if (p->kind == P_HGRENADE) explode(p->o, skill_get("sk_plr_hand_grenade"), p->owner);
            else if (p->kind == P_SNARK) { snd_emit(-1, CHAN_VOICE, "squeek/sqk_blast1.wav", 1, ATTN_NORM, PITCH_NORM, p->o); game_radius_damage(p->o, skill_get("sk_snark_dmg_pop"), 15 * 2.5f, -1, DMG_BLAST); }
            p->kind = P_NONE;
            continue;
        }
        if (p->kind == P_TRIPMINE) {
            if (wtime < p->armed) continue;
            if (!p->stuck) {
                // arm: measure the beam (CTripmineGrenade::PowerupThink / MakeBeam)
                // start just off the wall the mine sits on (map-placed mines are flush with it)
                for (int k = 0; k < 3; k++) p->o[k] += p->v[k] * 2;
                float end[3] = {p->o[0] + p->v[0] * 2048, p->o[1] + p->v[1] * 2048, p->o[2] + p->v[2] * 2048};
                gtrace_t tr;
                game_trace(p->o, end, NULL, NULL, TR_IGNORE_MONSTERS | TR_IGNORE_PLAYER, -1, &tr);
                memcpy(p->beam_end, tr.endpos, 12);
                debugf("tripmine armed at %.0f %.0f %.0f dir %.2f %.2f %.2f end %.0f %.0f %.0f%s\n", p->o[0], p->o[1], p->o[2], p->v[0], p->v[1], p->v[2], tr.endpos[0], tr.endpos[1], tr.endpos[2], tr.startsolid ? " (start solid)" : "");
                p->stuck = true;
                snd_emit(-1, CHAN_VOICE, "weapons/mine_activate.wav", 0.5f, ATTN_NORM, 75, p->o);
            }
            add_beam(p->o, p->beam_end, 255, 40, 40, dt * 1.5f, 1);
            gtrace_t tr;
            game_trace(p->o, p->beam_end, NULL, NULL, 0, -1, &tr);
            if (tr.fraction < 0.99f && (tr.player || tr.npc >= 0)) {
                explode(p->o, skill_get("sk_plr_tripmine"), p->owner);
                p->kind = P_NONE;
            }
            continue;
        }
        if (p->kind == P_SATCHEL && p->stuck) continue;
        // homing
        if (p->kind == P_HORNET || p->kind == P_HORNET_P || p->kind == P_SNARK || (p->kind == P_BALL && p->target >= 0) ||
            p->kind == P_TELEBALL || p->kind == P_NIHZAP) {
            if (p->target < 0 || (p->target > 0 && !npc_alive(p->target - 1))) p->target = nearest_target(p->o, p->owner, p->kind == P_SNARK ? 1024 : 512);
            if (p->target >= 0) {
                float t[3];
                target_point(p->target, t);
                float d[3] = {t[0] - p->o[0], t[1] - p->o[1], t[2] - p->o[2]};
                float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                float sp = sqrtf(p->v[0] * p->v[0] + p->v[1] * p->v[1] + p->v[2] * p->v[2]);
                if (l > 1 && sp > 1) {
                    float turn = p->kind == P_SNARK ? 4 : 6;
                    for (int k = 0; k < 3; k++) p->v[k] += (d[k] / l * sp - p->v[k]) * fminf(1, turn * dt);
                    if (p->kind == P_SNARK) p->v[2] = p->v[2] * 0.5f;
                }
            }
        }
        // rockets follow the laser dot (CRpgRocket::FollowThink)
        if (p->kind == P_ROCKET && cur == WEAPON_RPG && rpg_laser && p->owner == 0) {
            float eye[3], f[3];
            player_get_eye(eye);
            weapons_aim(f);
            float end[3] = {eye[0] + f[0] * 8192, eye[1] + f[1] * 8192, eye[2] + f[2] * 8192};
            gtrace_t tr;
            game_trace(eye, end, NULL, NULL, TR_IGNORE_PLAYER, -1, &tr);
            float d[3] = {tr.endpos[0] - p->o[0], tr.endpos[1] - p->o[1], tr.endpos[2] - p->o[2]};
            float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            float sp = sqrtf(p->v[0] * p->v[0] + p->v[1] * p->v[1] + p->v[2] * p->v[2]);
            if (l > 1) for (int k = 0; k < 3; k++) p->v[k] = p->v[k] * 0.8f + d[k] / l * sp * 0.2f * 1.0f + d[k] / l * 0.0f;
        }
        if (p->kind == P_ROCKET) {
            float sp = sqrtf(p->v[0] * p->v[0] + p->v[1] * p->v[1] + p->v[2] * p->v[2]);
            if (sp < 2000 && sp > 0.1f) for (int k = 0; k < 3; k++) p->v[k] *= (sp + 1200 * dt) / sp;
        }
        // gravity for thrown things
        if (p->kind == P_HGRENADE || p->kind == P_ARGRENADE || p->kind == P_SATCHEL || p->kind == P_SNARK || p->kind == P_SPIT || p->kind == P_MORTAR)
            p->v[2] -= (p->kind == P_ARGRENADE ? 400 : 800) * dt;
        if (p->kind == P_BOLT && pm_get_waterlevel() < 3) p->v[2] -= 40 * dt;
        float to[3] = {p->o[0] + p->v[0] * dt, p->o[1] + p->v[1] * dt, p->o[2] + p->v[2] * dt};
        static const float smn[3] = {-2, -2, -2}, smx[3] = {2, 2, 2};
        gtrace_t tr;
        int flags = p->owner == 0 ? TR_IGNORE_PLAYER : 0;
        int ignore = p->owner > 0 ? p->owner - 1 : -1;
        game_trace(p->o, to, (p->kind == P_SNARK || p->kind == P_SATCHEL || p->kind == P_HGRENADE) ? smn : NULL,
                   (p->kind == P_SNARK || p->kind == P_SATCHEL || p->kind == P_HGRENADE) ? smx : NULL, flags, ignore, &tr);
        if (tr.startsolid) tr.fraction = 0;
        memcpy(p->o, tr.endpos, 12);
        if (tr.fraction >= 1) {
            if (p->kind == P_ROCKET && rand() % 3 == 0) add_beam(p->o, to, 160, 160, 160, 0.4f, 2);
            continue;
        }
        // hit something
        float sp = sqrtf(p->v[0] * p->v[0] + p->v[1] * p->v[1] + p->v[2] * p->v[2]);
        switch (p->kind) {
        case P_ARGRENADE:
            explode(p->o, skill_get("sk_plr_9mmAR_grenade"), p->owner);
            p->kind = P_NONE;
            break;
        case P_ROCKET:
            explode(p->o, skill_get("sk_plr_rpg"), p->owner);
            p->kind = P_NONE;
            break;
        case P_BOLT:
            if (tr.npc >= 0 || tr.player) {
                proj_hit_damage(p, &tr, skill_get("sk_plr_xbow_bolt_monster"), DMG_BULLET | DMG_NEVERGIB);
                snd_emit(-1, CHAN_BODY, rand() & 1 ? "weapons/xbow_hitbod1.wav" : "weapons/xbow_hitbod2.wav", 1, ATTN_NORM, PITCH_NORM, p->o);
                p->kind = P_NONE;
            } else {
                if (tr.ent > 0) ents_damage_brush(tr.ent, skill_get("sk_plr_xbow_bolt_monster"), DMG_BULLET, p->v);
                snd_emit(-1, CHAN_BODY, "weapons/xbow_hit1.wav", 1, ATTN_NORM, 98 + rand() % 7, p->o);
                p->v[0] = p->v[1] = p->v[2] = 0;
                p->die = wtime + 10;
                p->kind = P_BOLT;
                p->stuck = true;
            }
            break;
        case P_HORNET:
        case P_HORNET_P:
            if (tr.npc >= 0 || tr.player) {
                proj_hit_damage(p, &tr, skill_get("sk_hornet_dmg") > 0 ? skill_get("sk_hornet_dmg") : 7, DMG_BULLET);
                snd_emit(-1, CHAN_VOICE, "hornet/ag_hornethit1.wav", 1, ATTN_NORM, PITCH_NORM, p->o);
                p->kind = P_NONE;
            } else {
                // bounce off walls
                float dn = p->v[0] * tr.normal[0] + p->v[1] * tr.normal[1] + p->v[2] * tr.normal[2];
                for (int k = 0; k < 3; k++) p->v[k] -= 2 * dn * tr.normal[k];
            }
            break;
        case P_SPIT:
            snd_emit(-1, CHAN_VOICE, "bullchicken/bc_spithit1.wav", 1, ATTN_NORM, PITCH_NORM, p->o);
            proj_hit_damage(p, &tr, skill_get("sk_bullsquid_dmg_spit"), DMG_GENERIC);
            p->kind = P_NONE;
            break;
        case P_BALL:
            proj_hit_damage(p, &tr, skill_get("sk_controller_dmgball"), DMG_ENERGYBEAM);
            p->kind = P_NONE;
            break;
        case P_MORTAR:   // CBMortar::Touch: an acid splash
            snd_emit(-1, CHAN_VOICE, "bullchicken/bc_spithit1.wav", 1, ATTN_NORM, 90, p->o);
            game_effect_particles(p->o, 14, 260, 120, 220, 60, 0.7f, 600);
            game_radius_damage(p->o, skill_get("sk_bigmomma_dmg_blast"), skill_get("sk_bigmomma_radius_blast") > 0 ? skill_get("sk_bigmomma_radius_blast") : 250,
                               p->owner > 0 ? p->owner - 1 : -1, DMG_ACID);
            p->kind = P_NONE;
            break;
        case P_NIHZAP:   // CNihilanthHVR::ZapTouch
            snd_emit(-1, CHAN_VOICE, "weapons/electro4.wav", 1, ATTN_NORM, PITCH_NORM, p->o);
            proj_hit_damage(p, &tr, skill_get("sk_nihilanth_zap") > 0 ? skill_get("sk_nihilanth_zap") : 30, DMG_SHOCK);
            game_effect_particles(p->o, 8, 200, 255, 240, 160, 0.4f, 300);
            p->kind = P_NONE;
            break;
        case P_TELEBALL:   // CNihilanthHVR::TeleportTouch: the player is sent to the next arena
            if (tr.player) { extern void npc_teleball_hit(void); npc_teleball_hit(); }
            snd_emit(-1, CHAN_VOICE, "x/x_teleattack1.wav", 1, ATTN_NORM, PITCH_NORM, p->o);
            p->kind = P_NONE;
            break;
        case P_SNARK:
            if (tr.npc >= 0 || tr.player) {
                proj_hit_damage(p, &tr, skill_get("sk_snark_dmg_bite"), DMG_SLASH);
                char s[32];
                snprintf(s, sizeof(s), "squeek/sqk_deploy1.wav");
                snd_emit(-1, CHAN_VOICE, s, 1, ATTN_NORM, 100 + rand() % 30, p->o);
                p->v[2] = 300;
            } else if (tr.normal[2] > 0.5f) {
                // hop along the floor
                p->v[2] = 180 + rand() % 100;
                if (rand() % 6 == 0) snd_emit(-1, CHAN_VOICE, "squeek/sqk_hunt1.wav", 0.6f, ATTN_NORM, 100 + rand() % 40, p->o);
            } else {
                float dn = p->v[0] * tr.normal[0] + p->v[1] * tr.normal[1] + p->v[2] * tr.normal[2];
                for (int k = 0; k < 3; k++) p->v[k] -= 1.5f * dn * tr.normal[k];
            }
            break;
        default: {
            // grenades and satchels bounce (CGrenade::BounceTouch)
            float dn = p->v[0] * tr.normal[0] + p->v[1] * tr.normal[1] + p->v[2] * tr.normal[2];
            for (int k = 0; k < 3; k++) p->v[k] = (p->v[k] - 2 * dn * tr.normal[k]) * 0.6f;
            if (tr.normal[2] > 0.7f && sp < 60) {
                p->v[0] = p->v[1] = p->v[2] = 0;
                if (p->kind == P_SATCHEL) p->stuck = true;
            } else if (sp > 80) {
                char s[32];
                snprintf(s, sizeof(s), p->kind == P_SATCHEL ? "weapons/g_bounce%d.wav" : "weapons/grenade_hit%d.wav", 1 + rand() % 3);
                snd_emit(-1, CHAN_VOICE, s, 0.25f, ATTN_NORM, PITCH_NORM, p->o);
            }
            break;
        }
        }
    }
}

static void satchels_detonate(void)
{
    for (int i = 0; i < MAX_PROJ; i++)
        if (projs[i].kind == P_SATCHEL && projs[i].owner == 0) { explode(projs[i].o, skill_get("sk_plr_satchel"), 0); projs[i].kind = P_NONE; }
    satchel_out = 0;
}

void weapons_clear_world(void)
{
    memset(projs, 0, sizeof(projs));
    memset(beams, 0, sizeof(beams));
    memset(flashes, 0, sizeof(flashes));
    memset(rings, 0, sizeof(rings));
    satchel_out = 0;
    egon_state = 0;
    gauss_charging = false;
}

// ------------------------------------------------------------------ inventory
bool weapons_give(const char *cls)
{
    for (int i = 1; i < NUM_W; i++) {
        const wdef_t *w = &W[i];
        if (!w->id || strcasecmp(w->cls, cls)) continue;
        bool had = player.weapons & (1u << w->id);
        bool took = false;
        if (w->ammo1 >= 0) {
            int give = w->give;
            if (!had && w->maxclip > 0) {
                int c = give < w->maxclip ? give : w->maxclip;
                player.clip[w->id] = c;
                give -= c;
            }
            if (player.ammo[w->ammo1] < ammo_max[w->ammo1] && give > 0) {
                player.ammo[w->ammo1] += give;
                if (player.ammo[w->ammo1] > ammo_max[w->ammo1]) player.ammo[w->ammo1] = ammo_max[w->ammo1];
                took = true;
            }
            if (!had && w->maxclip < 0 && w->give > 0) took = true;
        }
        if (!had) {
            player.weapons |= 1u << w->id;
            took = true;
            if (!NOPICKSND) snd_emit(0, CHAN_ITEM, "items/gunpickup2.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
            // switch to it when it's better than what we hold (CBasePlayer::AddPlayerItem -> SwitchWeapon)
            if (cur == 0 || (w->slot > W[cur].slot && w->id != WEAPON_SNARK && w->id != WEAPON_TRIPMINE && w->id != WEAPON_SATCHEL && w->id != WEAPON_HANDGRENADE))
                pending = w->id;
        } else if (took) {
            if (!NOPICKSND) snd_emit(0, CHAN_ITEM, "items/9mmclip1.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        }
        return took;
    }
    return false;
}

bool weapons_give_ammo(const char *cls)
{
    static const struct { const char *c; int type, n; } A[] = {
        {"ammo_9mmclip", AM_9MM, 17}, {"ammo_glockclip", AM_9MM, 17}, {"ammo_9mmAR", AM_9MM, 50}, {"ammo_mp5clip", AM_9MM, 50},
        {"ammo_9mmbox", AM_9MM, 200}, {"ammo_ARgrenades", AM_ARGREN, 2}, {"ammo_mp5grenades", AM_ARGREN, 2},
        {"ammo_buckshot", AM_BUCK, 12}, {"ammo_357", AM_357, 6}, {"ammo_crossbow", AM_BOLT, 5},
        {"ammo_rpgclip", AM_ROCKET, 1}, {"ammo_gaussclip", AM_URANIUM, 20}, {"ammo_egonclip", AM_URANIUM, 20},
    };
    for (unsigned i = 0; i < sizeof(A) / sizeof(A[0]); i++) {
        if (strcasecmp(A[i].c, cls)) continue;
        if (player.ammo[A[i].type] >= ammo_max[A[i].type]) return false;
        player.ammo[A[i].type] += A[i].n;
        if (player.ammo[A[i].type] > ammo_max[A[i].type]) player.ammo[A[i].type] = ammo_max[A[i].type];
        if (!NOPICKSND) snd_emit(0, CHAN_ITEM, "items/9mmclip1.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        return true;
    }
    if (!strcasecmp(cls, "weaponbox")) {
        // a dead soldier's box: some 9mm and grenades
        player.ammo[AM_9MM] = player.ammo[AM_9MM] + 50 > 250 ? 250 : player.ammo[AM_9MM] + 50;
        if (!NOPICKSND) snd_emit(0, CHAN_ITEM, "items/9mmclip1.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        return true;
    }
    return false;
}

static bool has_ammo(int id)
{
    const wdef_t *w = wdef(id);
    if (!w) return false;
    if (w->ammo1 < 0) return true;
    if (w->maxclip > 0 && player.clip[id] > 0) return true;
    return player.ammo[w->ammo1] > 0;
}

static void play_vseq(const char *name)
{
    if (!vmodel || !name) return;
    int s = studio_find_seq(vmodel, name);
    if (s < 0) return;
    vseq = s;
    vframe = 0;
    vseq_done = false;
}

static float vseq_len(const char *name)
{
    if (!vmodel || !name) return 0.5f;
    int s = studio_find_seq(vmodel, name);
    return s >= 0 ? studio_seq_duration(vmodel, s) : 0.5f;
}

// is there a free block of this size? (volatile: the compiler may not elide the probe)
static bool mem_available(int bytes)
{
    void *volatile p = malloc(bytes);
    if (!p) return false;
    free(p);
    return true;
}

// view models are big: keep only the one in hand (the trip mine's stays, it's also the placed mine)
static void release_vmodel(void)
{
    if (vmodel && strcmp(vmodel->name, "v_tripmine")) {
        bool used = false;
        for (int i = 0; i < MAX_PROJ; i++) if (projs[i].kind != P_NONE && projs[i].model == vmodel) used = true;
        if (!used) studio_free(vmodel);
    }
    vmodel = NULL;
}

static void deploy(int id)
{
    const wdef_t *w = wdef(id);
    cur = id;
    pending = 0;
    reloading = false;
    shotgun_reload_state = 0;
    egon_state = 0;
    gauss_charging = false;
    grenade_start = 0;
    if (!w) { release_vmodel(); return; }
    const char *vm = w->vmodel;
    if (id == WEAPON_SATCHEL && satchel_out > 0) vm = "v_satchel_radio";
    release_vmodel();
    // a view model needs up to ~250 KB: on the biggest maps there may not be room, and then
    // the weapon is used without its first-person model rather than running out of memory
    if (mem_available(320 * 1024)) vmodel = studio_get(vm);
    else debugf("weapons: no room for %s\n", vm);
    vbody = 0;
    play_vseq(w->s_draw);
    next_attack = next_attack2 = wtime + 0.5f;
    idle_time = wtime + vseq_len(w->s_draw);
    player.cur_weapon = id;
}

void weapons_reset(void)
{
    cur = pending = 0;
    vmodel = NULL;
    weapons_clear_world();
}

// after a map load: models were freed, re-get the view model
// something moved from a to b without walking (an NPC unstuck to its mark): set off any armed
// tripmine whose beam it would have crossed
void weapons_tripmine_sweep(const float *a, const float *b, float zlo, float zhi)
{
    for (int i = 0; i < MAX_PROJ; i++) {
        proj_t *p = &projs[i];
        if (p->kind != P_TRIPMINE || !p->stuck) continue;
        for (int s = 0; s <= 16; s++) {
            float q[3];
            for (int k = 0; k < 3; k++) q[k] = a[k] + (b[k] - a[k]) * s / 16.0f;
            // distance from q to the beam segment (2D) and height overlap
            float bx = p->beam_end[0] - p->o[0], by = p->beam_end[1] - p->o[1];
            float l2 = bx * bx + by * by;
            float t = l2 > 0 ? ((q[0] - p->o[0]) * bx + (q[1] - p->o[1]) * by) / l2 : 0;
            if (t < 0 || t > 1) continue;
            float cx = p->o[0] + bx * t - q[0], cy = p->o[1] + by * t - q[1];
            float bz = p->o[2] + (p->beam_end[2] - p->o[2]) * t;
            if (cx * cx + cy * cy < 16 * 16 && bz >= q[2] + zlo && bz <= q[2] + zhi) {
                explode(p->o, skill_get("sk_plr_tripmine"), p->owner);
                p->kind = P_NONE;
                break;
            }
        }
    }
}

void weapons_map_start(void)
{
    memset(gibs, 0, sizeof(gibs));   // the gib models are reloaded per map
    weapons_clear_world();
    int id = player.cur_weapon;
    vmodel = NULL;
    cur = 0;
    if (id && (player.weapons & (1u << id))) deploy(id);
    // tripmines the mapper placed (monster_tripmine: the "Lucky" scientists in c1a3a run into them)
    for (int i = ents_find_class("monster_tripmine", 0); i >= 0; i = ents_find_class("monster_tripmine", i + 1)) {
        entity_t *e = &entities[i];
        float p = e->angles[0] * (float)M_PI / 180, y = e->angles[1] * (float)M_PI / 180;
        float dir[3] = {cosf(p) * cosf(y), cosf(p) * sinf(y), -sinf(p)};
        int k = proj_spawn(P_TRIPMINE, e->origin, dir, -1, -1);
        if (k >= 0) projs[k].armed = wtime + 1.0f;
    }
}

int weapons_current(void) { return cur; }

static void select_next(int dir)
{
    // walk slots/positions like the HUD's weapon menu
    int order[NUM_W], n = 0;
    for (int s = 0; s < 5; s++)
        for (int p = 0; p < 5; p++)
            for (int i = 1; i < NUM_W; i++)
                if (W[i].id && W[i].slot == s && W[i].pos == p && (player.weapons & (1u << W[i].id))) order[n++] = W[i].id;
    if (!n) return;
    int at = 0;
    int base = pending ? pending : cur;
    for (int i = 0; i < n; i++) if (order[i] == base) at = i;
    int pick = order[(at + dir + n) % n];
    pending = pick;
    select_show = 1.5f;
    select_slot = W[pick].slot;
    snd_emit(0, CHAN_ITEM, "common/wpn_moveselect.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
}

// ------------------------------------------------------------------ aiming
static float aim_dir[3];
void weapons_aim(float *f) { memcpy(f, aim_dir, 12); }

// autoaim (CBasePlayer::GetAutoaimVector, sv_aim 1 in single player)
static void autoaim(const float *eye, float *dir)
{
    float best = 0.995f;   // ~ AUTOAIM_8DEGREES
    int bi = -1;
    float bv[3];
    for (int i = 0; i < num_npcs; i++) {
        npc_t *n = &npcs[i];
        if (!npc_alive(i) || (n->flags & 32)) continue;   // never at friends
        float c[3] = {n->origin[0], n->origin[1], n->origin[2] + (n->mins[2] + n->maxs[2]) * 0.6f};
        float d[3] = {c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]};
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l > 2048 || l < 1) continue;
        float dot = (d[0] * dir[0] + d[1] * dir[1] + d[2] * dir[2]) / l;
        if (dot <= best) continue;
        if (!game_visible(eye, c)) continue;
        best = dot; bi = i;
        for (int k = 0; k < 3; k++) bv[k] = d[k] / l;
    }
    if (bi >= 0) memcpy(dir, bv, 12);
}

static void fire_bullets(int shots, float spread_x, float spread_y, float dmg, float range, int type)
{
    float eye[3];
    player_get_eye(eye);
    float dir[3];
    memcpy(dir, aim_dir, 12);
    autoaim(eye, dir);
    // muzzle flash at the first-person gun's muzzle attachment (EV_MuzzleFlash / event 5001)
    add_view_flash(shots > 1 ? 4.5f : 3.5f);
    // right/up vectors from the aim direction
    float r[3] = {dir[1], -dir[0], 0};
    float rl = sqrtf(r[0] * r[0] + r[1] * r[1]);
    if (rl < 0.01f) { r[0] = 1; r[1] = 0; rl = 1; }
    r[0] /= rl; r[1] /= rl;
    float u[3] = {r[1] * dir[2] - r[2] * dir[1], r[2] * dir[0] - r[0] * dir[2], r[0] * dir[1] - r[1] * dir[0]};
    for (int s = 0; s < shots; s++) {
        float x = ((rand() % 1000) / 1000.0f - 0.5f + (rand() % 1000) / 1000.0f - 0.5f);
        float y = ((rand() % 1000) / 1000.0f - 0.5f + (rand() % 1000) / 1000.0f - 0.5f);
        float d[3];
        for (int k = 0; k < 3; k++) d[k] = dir[k] + x * spread_x * r[k] + y * spread_y * u[k];
        float end[3] = {eye[0] + d[0] * range, eye[1] + d[1] * range, eye[2] + d[2] * range};
        gtrace_t tr;
        game_trace(eye, end, NULL, NULL, TR_IGNORE_PLAYER | TR_HIT_CORPSES, -1, &tr);
        if (tr.fraction >= 1) continue;
        if (tr.npc >= 0) { { extern void npc_set_hitpoint(const float *p); npc_set_hitpoint(tr.endpos); } npc_take_damage(tr.npc, dmg, type, d, 0); }
        else { if (tr.ent > 0) ents_damage_brush(tr.ent, dmg, type, d); effect_sprite(tr.endpos, 1, 200, 200, 190, 0.5f); game_effect_particles(tr.endpos, 2, 70, 255, 220, 140, 0.3f, 300); }
        if (tr.npc < 0 && rand() % 4 == 0) {
            char sn[32];
            snprintf(sn, sizeof(sn), "weapons/ric%d.wav", 1 + rand() % 5);
            snd_emit(-1, CHAN_STATIC, sn, 0.5f, ATTN_NORM, PITCH_NORM, tr.endpos);
        }
    }
}

static void punch(float pitch)
{
    float p[3];
    pm_get_punch(p);
    p[0] += pitch;
    pm_set_punch(p);
}

static int clip_or_ammo(const wdef_t *w) { return w->maxclip > 0 ? player.clip[w->id] : player.ammo[w->ammo1]; }
static void use_ammo(const wdef_t *w, int n)
{
    if (w->maxclip > 0) player.clip[w->id] -= n;
    else player.ammo[w->ammo1] -= n;
}

static void dry_fire(void)
{
    snd_emit(0, CHAN_WEAPON, "weapons/357_cock1.wav", 0.8f, ATTN_NORM, PITCH_NORM, NULL);
    next_attack = wtime + 0.2f;
}

static void start_reload(const wdef_t *w, float time)
{
    if (w->maxclip <= 0 || player.clip[w->id] >= w->maxclip || player.ammo[w->ammo1] <= 0) return;
    if (w->id == WEAPON_SHOTGUN) {
        // CShotgun::Reload: start, then one shell at a time
        play_vseq("start_reload");
        shotgun_reload_state = 1;
        next_attack = next_attack2 = wtime + 0.6f;
        reload_end = wtime + 0.6f;
        return;
    }
    play_vseq(w->s_reload);
    reloading = true;
    reload_end = wtime + time;
    next_attack = next_attack2 = wtime + time;
}

static void finish_reload(const wdef_t *w)
{
    int need = w->maxclip - player.clip[w->id];
    int take = need < player.ammo[w->ammo1] ? need : player.ammo[w->ammo1];
    player.clip[w->id] += take;
    player.ammo[w->ammo1] -= take;
    reloading = false;
}

static void crowbar_swing(void)
{
    // CCrowbar::Swing: a 32-unit trace (then a hull trace) in front of the eye
    float eye[3];
    player_get_eye(eye);
    float end[3] = {eye[0] + aim_dir[0] * 32, eye[1] + aim_dir[1] * 32, eye[2] + aim_dir[2] * 32};
    gtrace_t tr;
    game_trace(eye, end, NULL, NULL, TR_IGNORE_PLAYER | TR_HIT_CORPSES, -1, &tr);
    if (tr.fraction >= 1) {
        static const float mn[3] = {-16, -16, -18}, mx[3] = {16, 16, 18};
        game_trace(eye, end, mn, mx, TR_IGNORE_PLAYER | TR_HIT_CORPSES, -1, &tr);
    }
    static int swing;
    swing++;
#ifdef TESTCROWBAR
    debugf("SWING eye %.0f %.0f %.0f frac %.2f npc %d ent %d\n", eye[0], eye[1], eye[2], tr.fraction, tr.npc, tr.ent);
#endif
    if (tr.fraction >= 1) {
        // v_crowbar: attack1 = 1 hit, attack1miss, attack2 = 2 miss, attack2hit, attack3 = 3 miss, attack3hit
        play_vseq(swing % 3 == 0 ? "attack1miss" : (swing % 3 == 1 ? "attack2" : "attack3"));
        snd_emit(0, CHAN_WEAPON, "weapons/cbar_miss1.wav", 1, ATTN_NORM, 94 + rand() % 15, NULL);
        next_attack = wtime + 0.5f;
        return;
    }
    play_vseq(swing % 3 == 0 ? "attack1" : (swing % 3 == 1 ? "attack2hit" : "attack3hit"));
    float dmg = skill_get("sk_plr_crowbar");
    if (tr.npc >= 0) {
        npc_take_damage(tr.npc, dmg, DMG_CLUB, aim_dir, 0);
        char s[32];
        snprintf(s, sizeof(s), "weapons/cbar_hitbod%d.wav", 1 + rand() % 3);
        snd_emit(0, CHAN_ITEM, s, 1, ATTN_NORM, PITCH_NORM, NULL);
    } else {
        if (tr.ent > 0) ents_damage_brush(tr.ent, dmg, DMG_CLUB, aim_dir);
        snd_emit(0, CHAN_ITEM, rand() & 1 ? "weapons/cbar_hit1.wav" : "weapons/cbar_hit2.wav", 1, ATTN_NORM, 98 + rand() % 4, NULL);
    }
    next_attack = wtime + 0.25f;
}

static void throw_dir(float *d, float up)
{
    memcpy(d, aim_dir, 12);
    d[2] += up;
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int k = 0; k < 3; k++) d[k] /= l;
}

static void primary(const wdef_t *w)
{
    float eye[3];
    player_get_eye(eye);
    switch (w->id) {
    case WEAPON_CROWBAR:
        crowbar_swing();
        npc_hear(eye, 200, 0);
        return;
    case WEAPON_GLOCK:
        if (player.clip[w->id] <= 0) { dry_fire(); return; }
        use_ammo(w, 1);
        play_vseq(player.clip[w->id] > 0 ? "shoot" : "shoot_empty");
        snd_emit(0, CHAN_WEAPON, w->snd, 1, ATTN_NORM, 98 + rand() % 4, NULL);
        fire_bullets(1, 0.01f, 0.01f, skill_get("sk_plr_9mm_bullet"), 8192, DMG_BULLET);
        punch(-2);
        next_attack = wtime + 0.3f;
        muzzle_flash = 2;
        npc_hear(eye, 600, 0);
        break;
    case WEAPON_PYTHON:
        if (player.clip[w->id] <= 0) { dry_fire(); return; }
        use_ammo(w, 1);
        play_vseq("fire1");
        snd_emit(0, CHAN_WEAPON, rand() & 1 ? "weapons/357_shot1.wav" : "weapons/357_shot2.wav", 0.8f, ATTN_NORM, PITCH_NORM, NULL);
        fire_bullets(1, 0.0175f, 0.0175f, skill_get("sk_plr_357_bullet"), 8192, DMG_BULLET);
        punch(-10);
        next_attack = wtime + 0.75f;
        muzzle_flash = 2;
        npc_hear(eye, 1000, 0);
        break;
    case WEAPON_MP5: {
        if (pm_get_waterlevel() == 3 || player.clip[w->id] <= 0) { dry_fire(); next_attack = wtime + 0.15f; return; }
        use_ammo(w, 1);
        play_vseq("shoot");
        char s[32];
        snprintf(s, sizeof(s), "weapons/hks%d.wav", 1 + rand() % 3);
        snd_emit(0, CHAN_WEAPON, s, 1, ATTN_NORM, 94 + rand() % 15, NULL);
        fire_bullets(1, 0.10461f, 0.10461f, skill_get("sk_plr_9mmAR_bullet"), 8192, DMG_BULLET);
        punch(-(rand() % 200) / 100.0f);
        next_attack = wtime + 0.1f;
        muzzle_flash = 1;
        npc_hear(eye, 1000, 0);
        break;
    }
    case WEAPON_SHOTGUN:
        if (pm_get_waterlevel() == 3 || player.clip[w->id] <= 0) { if (player.clip[w->id] <= 0) start_reload(w, 0); else dry_fire(); return; }
        use_ammo(w, 1);
        shotgun_reload_state = 0;
        play_vseq("shoot");
        snd_emit(0, CHAN_WEAPON, "weapons/sbarrel1.wav", 0.95f, ATTN_NORM, 93 + rand() % 31, NULL);
        fire_bullets(6, 0.17365f, 0.17365f, skill_get("sk_plr_buckshot"), 2048, DMG_BULLET);
        punch(-5);
        next_attack = next_attack2 = wtime + 0.75f;
        muzzle_flash = 2;
        npc_hear(eye, 1000, 0);
        break;
    case WEAPON_CROSSBOW: {
        if (player.clip[w->id] <= 0) { dry_fire(); return; }
        use_ammo(w, 1);
        play_vseq(player.clip[w->id] > 0 ? "fire1" : "fire3");
        snd_emit(0, CHAN_WEAPON, "weapons/xbow_fire1.wav", 1, ATTN_NORM, 93 + rand() % 15, NULL);
        float o[3] = {eye[0] + aim_dir[0] * 16, eye[1] + aim_dir[1] * 16, eye[2] + aim_dir[2] * 16 - 2};
        float d[3];
        memcpy(d, aim_dir, 12);
        autoaim(eye, d);
        proj_spawn(P_BOLT, o, d, 0, -1);
        punch(-2);
        next_attack = wtime + 0.75f;
        npc_hear(eye, 200, 0);
        break;
    }
    case WEAPON_RPG: {
        if (player.clip[w->id] <= 0) { dry_fire(); return; }
        use_ammo(w, 1);
        play_vseq("fire");
        snd_emit(0, CHAN_WEAPON, "weapons/rocketfire1.wav", 0.9f, ATTN_NORM, PITCH_NORM, NULL);
        float o[3] = {eye[0] + aim_dir[0] * 16, eye[1] + aim_dir[1] * 16, eye[2] + aim_dir[2] * 16 - 8};
        proj_spawn(P_ROCKET, o, aim_dir, 0, -1);
        punch(-5);
        next_attack = wtime + 1.5f;
        npc_hear(eye, 1000, 0);
        break;
    }
    case WEAPON_GAUSS: {
        if (player.ammo[w->ammo1] < 2) { dry_fire(); return; }
        player.ammo[w->ammo1] -= 2;
        play_vseq("fire2");
        snd_emit(0, CHAN_WEAPON, "weapons/gauss2.wav", 0.5f + (rand() % 30) / 100.0f, ATTN_NORM, 85 + rand() % 15, NULL);
        float end[3] = {eye[0] + aim_dir[0] * 8192, eye[1] + aim_dir[1] * 8192, eye[2] + aim_dir[2] * 8192};
        gtrace_t tr;
        game_trace(eye, end, NULL, NULL, TR_IGNORE_PLAYER | TR_HIT_CORPSES, -1, &tr);
        float from[3] = {eye[0], eye[1], eye[2] - 6};
        add_beam(from, tr.endpos, 255, 128, 0, 0.1f, 2);
        if (tr.npc >= 0) npc_take_damage(tr.npc, skill_get("sk_plr_gauss"), DMG_BULLET, aim_dir, 0);
        else if (tr.ent > 0) ents_damage_brush(tr.ent, skill_get("sk_plr_gauss"), DMG_BULLET, aim_dir);
        punch(-2);
        next_attack = next_attack2 = wtime + 0.2f;
        npc_hear(eye, 1000, 0);
        break;
    }
    case WEAPON_HORNETGUN: {
        if (player.ammo[w->ammo1] <= 0) return;
        player.ammo[w->ammo1]--;
        play_vseq("Shoot");
        char s[32];
        snprintf(s, sizeof(s), "agrunt/ag_fire%d.wav", 1 + rand() % 3);
        snd_emit(0, CHAN_WEAPON, s, 1, ATTN_NORM, 100, NULL);
        float o[3] = {eye[0] + aim_dir[0] * 16, eye[1] + aim_dir[1] * 16, eye[2] + aim_dir[2] * 16 - 12};
        int p = proj_spawn(P_HORNET_P, o, aim_dir, 0, -1);
        (void)p;
        punch(-(rand() % 200) / 100.0f);
        next_attack = wtime + 0.25f;
        hornet_regen = wtime + 0.5f;
        npc_hear(eye, 600, 0);
        break;
    }
    case WEAPON_HANDGRENADE:
        if (!grenade_start && player.ammo[w->ammo1] > 0) { grenade_start = wtime; play_vseq("pinpull"); next_attack = wtime + 0.5f; }
        return;
    case WEAPON_SATCHEL:
        if (satchel_out > 0) {
            // the radio: blow them all (CSatchel::PrimaryAttack)
            play_vseq("fire");
            satchels_detonate();
            next_attack = wtime + 0.5f;
            break;
        }
        // fall through: throw one
    case -WEAPON_SATCHEL: {
        if (player.ammo[AM_SATCHEL] <= 0) return;
        player.ammo[AM_SATCHEL]--;
        float d[3];
        throw_dir(d, 0.1f);
        float o[3] = {eye[0] + d[0] * 16, eye[1] + d[1] * 16, eye[2] - 8};
        proj_spawn(P_SATCHEL, o, d, 0, -1);
        satchel_out++;
        release_vmodel();
        if (mem_available(200 * 1024)) vmodel = studio_get("v_satchel_radio");
        play_vseq("draw");
        next_attack = next_attack2 = wtime + 1.0f;
        break;
    }
    case WEAPON_TRIPMINE: {
        if (player.ammo[w->ammo1] <= 0) return;
        float end[3] = {eye[0] + aim_dir[0] * 128, eye[1] + aim_dir[1] * 128, eye[2] + aim_dir[2] * 128};
        gtrace_t tr;
        game_trace(eye, end, NULL, NULL, TR_IGNORE_PLAYER | TR_HIT_CORPSES, -1, &tr);
        if (tr.fraction >= 1 || tr.npc >= 0) return;
        player.ammo[w->ammo1]--;
        float o[3] = {tr.endpos[0] + tr.normal[0] * 8, tr.endpos[1] + tr.normal[1] * 8, tr.endpos[2] + tr.normal[2] * 8};
        int p = proj_spawn(P_TRIPMINE, o, tr.normal, 0, -1);
        if (p >= 0) { memcpy(projs[p].v, tr.normal, 12); projs[p].yaw = atan2f(tr.normal[1], tr.normal[0]) * 180 / (float)M_PI; }
        snd_emit(0, CHAN_WEAPON, "weapons/mine_deploy.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        snd_emit(-1, CHAN_BODY, "weapons/mine_charge.wav", 0.2f, ATTN_NORM, PITCH_NORM, o);
        play_vseq("place");
        next_attack = wtime + 0.3f;
        break;
    }
    case WEAPON_SNARK: {
        if (player.ammo[w->ammo1] <= 0) return;
        player.ammo[w->ammo1]--;
        float d[3];
        throw_dir(d, 0.05f);
        float o[3] = {eye[0] + d[0] * 24, eye[1] + d[1] * 24, eye[2] - 4};
        proj_spawn(P_SNARK, o, d, 0, -1);
        snd_emit(0, CHAN_VOICE, "squeek/sqk_hunt2.wav", 1, ATTN_NORM, 105, NULL);
        play_vseq("throw");
        next_attack = wtime + 0.3f;
        break;
    }
    case WEAPON_EGON:
        break;
    }
    if (w->id != WEAPON_EGON && w->id != WEAPON_HANDGRENADE) idle_time = wtime + 2 + (rand() % 100) / 50.0f;
}

static void secondary(const wdef_t *w)
{
    float eye[3];
    player_get_eye(eye);
    switch (w->id) {
    case WEAPON_GLOCK:
        if (player.clip[w->id] <= 0) { dry_fire(); return; }
        use_ammo(w, 1);
        play_vseq("shoot");
        snd_emit(0, CHAN_WEAPON, w->snd, 1, ATTN_NORM, 98 + rand() % 4, NULL);
        fire_bullets(1, 0.1f, 0.1f, skill_get("sk_plr_9mm_bullet"), 8192, DMG_BULLET);
        punch(-2);
        next_attack = next_attack2 = wtime + 0.2f;
        muzzle_flash = 2;
        npc_hear(eye, 600, 0);
        break;
    case WEAPON_MP5: {
        if (player.ammo[AM_ARGREN] <= 0 || pm_get_waterlevel() == 3) { dry_fire(); next_attack2 = wtime + 0.15f; return; }
        player.ammo[AM_ARGREN]--;
        play_vseq("grenade");
        snd_emit(0, CHAN_WEAPON, rand() & 1 ? "weapons/glauncher.wav" : "weapons/glauncher2.wav", 0.8f, ATTN_NORM, PITCH_NORM, NULL);
        float o[3] = {eye[0] + aim_dir[0] * 16, eye[1] + aim_dir[1] * 16, eye[2] + aim_dir[2] * 16};
        proj_spawn(P_ARGRENADE, o, aim_dir, 0, -1);
        punch(-10);
        next_attack = next_attack2 = wtime + 1.0f;
        npc_hear(eye, 1000, 0);
        break;
    }
    case WEAPON_SHOTGUN:
        if (player.clip[w->id] < 2) { primary(w); return; }
        use_ammo(w, 2);
        shotgun_reload_state = 0;
        play_vseq("shoot_big");
        snd_emit(0, CHAN_WEAPON, "weapons/dbarrel1.wav", 1, ATTN_NORM, 85 + rand() % 31, NULL);
        fire_bullets(12, 0.17365f, 0.17365f, skill_get("sk_plr_buckshot"), 2048, DMG_BULLET);
        punch(-10);
        next_attack = next_attack2 = wtime + 1.5f;
        muzzle_flash = 2;
        npc_hear(eye, 1000, 0);
        break;
    case WEAPON_RPG:
        rpg_laser = !rpg_laser;
        snd_emit(0, CHAN_ITEM, "weapons/desert_eagle_sight.wav", 0.5f, ATTN_NORM, PITCH_NORM, NULL);
        next_attack2 = wtime + 0.2f;
        break;
    case WEAPON_HORNETGUN: {
        if (player.ammo[w->ammo1] <= 0) return;
        player.ammo[w->ammo1]--;
        play_vseq("Shoot");
        snd_emit(0, CHAN_WEAPON, "agrunt/ag_fire1.wav", 1, ATTN_NORM, 100, NULL);
        float o[3] = {eye[0] + aim_dir[0] * 16, eye[1] + aim_dir[1] * 16, eye[2] - 12};
        int p = proj_spawn(P_HORNET, o, aim_dir, 0, -2);
        if (p >= 0) { for (int k = 0; k < 3; k++) projs[p].v[k] *= 4; projs[p].target = -2; }
        next_attack = next_attack2 = wtime + 0.1f;
        hornet_regen = wtime + 0.5f;
        break;
    }
    case WEAPON_SATCHEL:
        primary(&(wdef_t){.id = -WEAPON_SATCHEL});
        break;
    case WEAPON_GAUSS:
        // charged shot: hold to charge, release to fire (CGauss::SecondaryAttack / StartFire)
        if (!gauss_charging) {
            if (player.ammo[w->ammo1] <= 0) { dry_fire(); return; }
            gauss_charging = true;
            gauss_charge_start = wtime;
            player.ammo[w->ammo1]--;
            play_vseq("spinup");
            snd_emit(0, CHAN_WEAPON, "ambience/pulsemachine.wav", 1, ATTN_NORM, 110, NULL);
        }
        break;
    default:
        break;
    }
}

static void gauss_release(const wdef_t *w)
{
    float eye[3];
    player_get_eye(eye);
    float t = wtime - gauss_charge_start;
    if (t > 4) t = 4;   // GAUSS_FULLCHARGE
    float dmg = 200 * t / 4;
    snd_stop(0, CHAN_WEAPON, "ambience/pulsemachine.wav");
    snd_emit(0, CHAN_WEAPON, "weapons/gauss2.wav", 1, ATTN_NORM, 85 + rand() % 15, NULL);
    play_vseq("fire");
    float end[3] = {eye[0] + aim_dir[0] * 8192, eye[1] + aim_dir[1] * 8192, eye[2] + aim_dir[2] * 8192};
    gtrace_t tr;
    game_trace(eye, end, NULL, NULL, TR_IGNORE_PLAYER | TR_HIT_CORPSES, -1, &tr);
    float from[3] = {eye[0], eye[1], eye[2] - 6};
    add_beam(from, tr.endpos, 255, 255, 255, 0.15f, 4);
    if (tr.npc >= 0) npc_take_damage(tr.npc, dmg, DMG_BULLET | DMG_ALWAYSGIB, aim_dir, 0);
    else if (tr.ent > 0) ents_damage_brush(tr.ent, dmg, DMG_BULLET, aim_dir);
    // the recoil pushes the player back
    float v[3];
    pm_get_velocity(v);
    for (int k = 0; k < 2; k++) v[k] -= aim_dir[k] * dmg * 5;
    pm_set_velocity(v);
    gauss_charging = false;
    next_attack = next_attack2 = wtime + 1.0f;
    npc_hear(eye, 1000, 0);
    (void)w;
}

static void egon_fire(const wdef_t *w, float dt)
{
    float eye[3];
    player_get_eye(eye);
    if (player.ammo[w->ammo1] <= 0 || pm_get_waterlevel() == 3) {
        if (egon_state) { egon_state = 0; snd_stop(0, CHAN_STATIC, NULL); snd_emit(0, CHAN_WEAPON, "weapons/egon_off1.wav", 0.98f, ATTN_NORM, 100, NULL); }
        return;
    }
    if (!egon_state) {
        egon_state = 1;
        snd_emit(0, CHAN_WEAPON, "weapons/egon_windup2.wav", 0.98f, ATTN_NORM, 125, NULL);
        snd_emit(0, CHAN_STATIC, "weapons/egon_run3.wav", 0.98f, ATTN_NORM, 125, NULL);
        play_vseq("fire3");
        egon_ammo_time = wtime;
    }
    if (wtime >= egon_ammo_time) { player.ammo[w->ammo1]--; egon_ammo_time = wtime + 0.2f; }   // EGON_PULSE_INTERVAL-ish
    float end[3] = {eye[0] + aim_dir[0] * 2048, eye[1] + aim_dir[1] * 2048, eye[2] + aim_dir[2] * 2048};
    gtrace_t tr;
    game_trace(eye, end, NULL, NULL, TR_IGNORE_PLAYER | TR_HIT_CORPSES, -1, &tr);
    // CEgon::CreateEffect: a wide noisy blue beam (lgtning) around a bright core (laserbeam), a
    // flare where it hits and sparks
    float from[3];
    if (vm_att_ok) memcpy(from, vm_att[0], 12);
    else {
        float rx = aim_dir[1], ry = -aim_dir[0], rl = sqrtf(rx * rx + ry * ry);
        if (rl > 0.01f) { rx /= rl; ry /= rl; }
        for (int k = 0; k < 3; k++) from[k] = eye[k] + aim_dir[k] * 18 + (k == 0 ? rx : k == 1 ? ry : 0) * 6 - (k == 2 ? 7 : 0);
    }
    // CEgon::CreateEffect/UpdateEffect: m_pBeam (xbeam1.spr, width 40, sine) and m_pNoise
    // (xbeam1.spr, width 55, noisy) from the gun's muzzle, the XSpark1 flare at the end
    float life = dt * 1.5f;
    float tb = fminf((wtime - egon_ammo_time + 0.2f) / 2.0f, 1.0f);   // colour shifts a little as it fires
    beam_world_next = true;
    beam_tex_next = 5;
    {
        float c = fabsf(sinf(wtime * 10));
        add_beam(from, tr.endpos, 60 + (int)(25 * tb), 120 + (int)(30 * tb), 64 + (int)(80 * c) + 100, life, 6.0f);
        // the noise beam: jittered segments around the line
        float dl[3], len = 0;
        for (int k = 0; k < 3; k++) { dl[k] = tr.endpos[k] - from[k]; len += dl[k] * dl[k]; }
        len = sqrtf(len);
        int segs = len > 256 ? 8 : len > 64 ? 5 : 2;
        float amp = fminf(len * 0.025f, 6);
        float prev[3];
        memcpy(prev, from, 12);
        for (int sgi = 1; sgi <= segs; sgi++) {
            float t = (float)sgi / segs, pt[3];
            for (int k = 0; k < 3; k++) pt[k] = from[k] + dl[k] * t + (sgi < segs ? ((rand() % 200) - 100) / 100.0f * amp * (t < 0.2f ? t * 5 : 1) : 0);
            add_beam(prev, pt, 80, 120, 255, life, 7.5f);
            memcpy(prev, pt, 12);
        }
    }
    beam_tex_next = 0;
    beam_world_next = false;
    add_bill(4, tr.endpos, 16, life, 255, 255, 255);
    if (rand() % 3 == 0) game_effect_particles(tr.endpos, 3, 160, 140, 160, 255, 0.35f, 300);
    float dmg = skill_get("sk_plr_egon_wide") * dt * 10;
    if (tr.npc >= 0) npc_take_damage(tr.npc, dmg, DMG_ENERGYBEAM | DMG_ALWAYSGIB, aim_dir, 0);
    else if (tr.ent > 0) ents_damage_brush(tr.ent, dmg, DMG_ENERGYBEAM, aim_dir);
    npc_hear(eye, 600, 0);
}

static void egon_stop(void)
{
    if (!egon_state) return;
    egon_state = 0;
    snd_stop(0, CHAN_STATIC, "weapons/egon_run3.wav");
    snd_emit(0, CHAN_WEAPON, "weapons/egon_off1.wav", 0.98f, ATTN_NORM, 100, NULL);
    play_vseq("idle1");
    next_attack = wtime + 0.5f;
}

void weapons_think(float dt, const float *view_angles, bool fire, bool fire2, bool reload_pressed, int select_dir,
                   bool can_fire)
{
    wtime += dt;
    bob_t += dt;
    if (select_show > 0) select_show -= dt;
    {
        float y = view_angles[1] * (float)M_PI / 180, p = view_angles[0] * (float)M_PI / 180;
        aim_dir[0] = cosf(p) * cosf(y); aim_dir[1] = cosf(p) * sinf(y); aim_dir[2] = -sinf(p);
        // auto-aim (CBasePlayer::GetAutoaimVector / the console versions): bend shots onto an enemy
        // close to the crosshair
        extern bool npc_autoaim(const float *eye, float *dir);
        float eye[3];
        pm_get_origin(eye);
        eye[2] += 28;
        npc_autoaim(eye, aim_dir);
    }
#ifdef TESTBEAM
    {   // beams across the view: lightning (top), laser (middle), gluon (bottom)
        static float nt = 3;
        if (wtime > nt) {
            nt = wtime + 0.1f;
            float o[3]; pm_get_origin(o);
            float y = view_angles[1] * (float)M_PI / 180, f[3] = {cosf(y), sinf(y), 0}, r[3] = {sinf(y), -cosf(y), 0};
            float a[3], c[3];
            for (int k = 0; k < 3; k++) { a[k] = o[k] + f[k] * 220 - r[k] * 160; c[k] = o[k] + f[k] * 220 + r[k] * 160; }
            a[2] += 70; c[2] += 70;
            game_effect_lightning(a, c, 140, 120, 255);
            a[2] -= 50; c[2] -= 50;
            game_effect_beam_plain(a, c, 255, 60, 60);
            a[2] -= 50; c[2] -= 50;
            beam_tex_next = 5; beam_world_next = true; add_beam(a, c, 160, 200, 255, 0.12f, 16); beam_tex_next = 0; beam_world_next = false;
        }
    }
#endif
#ifdef TESTRING
    {   // houndeye blasts in front of the camera
        static float nt = 4;
        if (wtime > nt) {
            nt = wtime + 0.6f;
            float o[3]; pm_get_origin(o);
            float y = view_angles[1] * (float)M_PI / 180;
            float c[3] = {o[0] + cosf(y) * 260, o[1] + sinf(y) * 260, o[2] - 36 + 8};
            game_effect_shockring(c, 384, 16, 0.3f, 101, 133, 221);
            game_effect_shockring(c, 192, 16, 0.3f, 101, 133, 221);
        }
    }
#endif
#ifdef TESTZAP
    {   // a vortigaunt zap from far away ending at the player (R_ZAP: game_effect_beam)
        static float nt = 4;
        if (wtime > nt) {
            nt = wtime + 0.5f;
            float o[3]; pm_get_origin(o);
            float y = view_angles[1] * (float)M_PI / 180, f[3] = {cosf(y), sinf(y), 0}, r[3] = {sinf(y), -cosf(y), 0};
            float a[3], c[3];
            for (int k = 0; k < 3; k++) { a[k] = o[k] + f[k] * 500 + r[k] * -120; c[k] = o[k] + r[k] * 6; }
            a[2] += 60; c[2] += 20;
            game_effect_beam(a, c, 96, 128, 16);
        }
    }
#endif
#ifdef TESTPROJ
    {   // effects test: the three PC-sprite projectiles drifting in front of the camera
        static float nt = 4;
        if (wtime > nt) {
            nt = wtime + 3;
            float o[3]; pm_get_origin(o);
            float y = view_angles[1] * (float)M_PI / 180;
            static const int kinds[3] = {P_TELEBALL, P_NIHZAP, P_BALL};
            for (int k = 0; k < 3; k++) {
                float sp[3] = {o[0] + cosf(y) * 260 - sinf(y) * (k - 1) * 90, o[1] + sinf(y) * 260 + cosf(y) * (k - 1) * 90, o[2] + 30};
                float v[3] = {-cosf(y) * 20, -sinf(y) * 20, 0};
                proj_spawn_vel(kinds[k], sp, v, -1, -1);
            }
        }
    }
#endif
    projs_think(dt);
    gibs_think(dt);
    // hornets grow back (CHgun::WeaponIdle / Reload)
    if ((player.weapons & (1u << WEAPON_HORNETGUN)) && player.ammo[AM_HORNET] < 8 && wtime > hornet_regen) {
        player.ammo[AM_HORNET]++;
        hornet_regen = wtime + 0.5f;
    }
    if (select_dir) select_next(select_dir);
    if (!player_alive()) { egon_stop(); return; }
    // switching: holster, then deploy (instant selection like the PC's HUD menu)
    if (pending && pending != cur && (fire || select_dir == 0)) {
        if (!(player.weapons & (1u << pending))) pending = 0;
        else if (select_show <= 1.0f || fire) { egon_stop(); deploy(pending); }
    }
    const wdef_t *w = wdef(cur);
#ifdef TESTVSEQ
    { static int ts = -1; static float tn = 5; if (vmodel && wtime > tn) { tn = wtime + 2.5f; ts = (ts + 1) % vmodel->numseqs; vseq = ts; vframe = 0; idle_time = wtime + 100; debugf("VSEQ %d %s\n", ts, vmodel->seqs[ts].name); } }
#endif
    if (vmodel && vseq >= 0 && vseq < vmodel->numseqs) {
        sseq_t *s = &vmodel->seqs[vseq];
        vframe += (s->fps > 0 ? s->fps : 10) * dt;
        if (vframe >= s->numframes - 1) {
            vseq_done = true;
            if (s->flags & 1) vframe = fmodf(vframe, s->numframes > 1 ? s->numframes - 1 : 1);
            else vframe = s->numframes - 1;
        }
    }
    if (!w) return;
    // reloads
    if (reloading && wtime >= reload_end) finish_reload(w);
    if (shotgun_reload_state && wtime >= reload_end) {
        if (fire && player.clip[w->id] > 0) { shotgun_reload_state = 0; }
        else if (player.clip[w->id] >= w->maxclip || player.ammo[w->ammo1] <= 0) {
            play_vseq("pump");
            snd_emit(0, CHAN_ITEM, "weapons/scock1.wav", 1, ATTN_NORM, 95 + rand() % 31, NULL);
            shotgun_reload_state = 0;
            next_attack = next_attack2 = wtime + 0.5f;
        } else {
            play_vseq("reload");
            snd_emit(0, CHAN_ITEM, rand() & 1 ? "weapons/reload1.wav" : "weapons/reload3.wav", 1, ATTN_NORM, 85 + rand() % 31, NULL);
            player.clip[w->id]++;
            player.ammo[w->ammo1]--;
            reload_end = wtime + 0.5f;
            shotgun_reload_state = 2;
        }
    }
    // hand grenade: throw on release (CHandGrenade::WeaponIdle)
    if (cur == WEAPON_HANDGRENADE && grenade_start && !fire && wtime >= next_attack) {
        float held = wtime - grenade_start;
        float d[3];
        throw_dir(d, 0.1f);
        float eye[3];
        player_get_eye(eye);
        float o[3] = {eye[0] + d[0] * 16, eye[1] + d[1] * 16, eye[2] + d[2] * 16};
        float vel = (90 - (-asinf(aim_dir[2]) * 180 / (float)M_PI) * 0 + 0) * 6.5f;
        if (vel > 500) vel = 500;
        int p = proj_spawn(P_HGRENADE, o, d, 0, -1);
        if (p >= 0) {
            for (int k = 0; k < 3; k++) projs[p].v[k] = d[k] * vel;
            float pv[3];
            pm_get_velocity(pv);
            for (int k = 0; k < 3; k++) projs[p].v[k] += pv[k];
            projs[p].die = grenade_start + 3.0f;   // the fuse started when the pin was pulled
            if (projs[p].die < wtime + 0.1f) projs[p].die = wtime + 0.1f;
        }
        play_vseq(held < 1 ? "throw1" : (held < 2 ? "throw2" : "throw3"));
        player.ammo[AM_HGREN]--;
        grenade_start = 0;
        next_attack = wtime + 0.5f;
        idle_time = wtime + 0.6f;
    }
    if (cur == WEAPON_HANDGRENADE && !grenade_start && player.ammo[AM_HGREN] <= 0 && wtime > next_attack) {
        player.weapons &= ~(1u << WEAPON_HANDGRENADE);
        select_next(1);
        if (pending == WEAPON_HANDGRENADE) pending = 0;
    }
    // gauss charge release
    if (cur == WEAPON_GAUSS && gauss_charging) {
        float t = wtime - gauss_charge_start;
        if (t > 10) {   // overcharged: it bites (CGauss::SecondaryAttack)
            snd_stop(0, CHAN_WEAPON, NULL);
            snd_emit(0, CHAN_ITEM, "weapons/electro4.wav", 1, ATTN_NORM, 80 + rand() % 20, NULL);
            player_take_damage(50, DMG_SHOCK, NULL, -1);
            gauss_charging = false;
            next_attack = next_attack2 = wtime + 1;
        } else if (!fire2) {
            gauss_release(w);
        } else if (t < 4 && player.ammo[w->ammo1] > 0) {
            static float use;
            if (wtime > use) { player.ammo[w->ammo1]--; use = wtime + 0.3f; }
        }
    }
    if (cur == WEAPON_EGON && (!fire || !can_fire)) egon_stop();
    if (!can_fire) return;
    if (fire && wtime >= next_attack && !reloading && !(shotgun_reload_state && player.clip[w->id] == 0)) {
        if (w->id == WEAPON_EGON) { if (wtime >= next_attack) egon_fire(w, dt); }
        else if (w->maxclip > 0 && player.clip[w->id] <= 0 && player.ammo[w->ammo1] > 0 && w->id != WEAPON_CROWBAR) start_reload(w, w->id == WEAPON_PYTHON ? 2.0f : (w->id == WEAPON_CROSSBOW ? 4.5f : (w->id == WEAPON_RPG ? 2.0f : 1.5f)));
        else primary(w);
    } else if (fire2 && wtime >= next_attack2 && !reloading) {
        secondary(w);
    } else if (reload_pressed && !reloading && !shotgun_reload_state) {
        start_reload(w, w->id == WEAPON_PYTHON ? 2.0f : (w->id == WEAPON_CROSSBOW ? 4.5f : (w->id == WEAPON_RPG ? 2.0f : 1.5f)));
    } else if (wtime > idle_time && !reloading && !shotgun_reload_state && !gauss_charging && !egon_state && !grenade_start) {
        // auto-reload an empty clip (CBasePlayerWeapon::ItemPostFrame), else idle animations
        if (w->maxclip > 0 && player.clip[w->id] == 0 && player.ammo[w->ammo1] > 0) start_reload(w, w->id == WEAPON_PYTHON ? 2.0f : 1.5f);
        else { play_vseq(w->s_idle); idle_time = wtime + 5 + rand() % 10; }
    }
    prev_fire = fire;
    prev_fire2 = fire2;
}

// ------------------------------------------------------------------ drawing
static float proj_z;   // depth of the last project(), for z-tested effects
#ifdef PROFX
static uint32_t pst[5], pacc[5]; static int pn_;
#define PSTAMP(i) (pst[i] = TICKS_READ())
#else
#define PSTAMP(i)
#endif
static bool project(const float *p, float *sx, float *sy)
{
    T3DVec3 in = {{p[0], p[1], p[2]}}, out;
    t3d_viewport_calc_viewspace_pos(render_viewport(), &out, &in);
    if (out.v[2] >= 1.0f || out.v[2] < 0) return false;
    // points right at the camera project to huge coordinates: a beam ending there would become
    // a screen-filling band (the cascade's zap beams), so treat them as off-screen
    if (fabsf(out.v[0]) > 1500 || fabsf(out.v[1]) > 1500) return false;
    *sx = out.v[0];
    *sy = out.v[1];
    proj_z = out.v[2];
    return true;
}

// world-space projectiles (models) and the first-person weapon; called by the renderer
static float fx_eye[3], fx_angles[3], fx_up[3] = {0, 0, 1};
bool weapons_hidden;   // manning a func_tank: no view model
// billboards (explosions, muzzle flashes, the gluon flare): one sprite upload per billboard.
// view = the first-person gun's own flashes, drawn over the view model at its muzzle
static sprite_t *fx_projspr[5];   // spit, momma spit, nihilanth zap, controller ball, teleport ball
// ---- env_sprite / env_glow / cycler_sprite with the PC's own sprite art (tools/spr2n64.py)
// spr/index.txt lists every sprite the maps use and the strip that draws it; strips are loaded the
// first time one is seen in a map (if the heap can spare it) and freed when the map changes.
typedef struct {
    char name[16], file[16];
    uint8_t frames, srcframes, rgba, type;
    uint8_t fw, fh, tint[3];
    uint16_t ow, oh;
    int8_t file_id;
} fxspr_t;
#define MAX_FXSPR 64
#define MAX_FXFILE 24
static fxspr_t fxspr[MAX_FXSPR];
static int num_fxspr;
static struct { char name[16]; sprite_t *sp; bool failed; } fxfile[MAX_FXFILE];
static int num_fxfile;
static uint32_t spr_col[MAX_ENTITIES];   // rendercolor + renderamt per sprite entity (0: not read yet)
static float spr_fr[MAX_ENTITIES];       // framerate
static uint8_t spr_vis[MAX_ENTITIES];    // glow sprites: 1 seen, 2 hidden (traced every few frames)
static void fxspr_load_index(void)
{
    FILE *f = fopen("rom:/spr/index.txt", "r");
    if (!f) return;
    char line[160];
    while (num_fxspr < MAX_FXSPR && fgets(line, sizeof line, f)) {
        fxspr_t *x = &fxspr[num_fxspr];
        char fmt[16];
        int nf, fw, fh, ow, oh, typ, tf, r, g, b, sn;
        if (sscanf(line, "%15s %15s %d %d %d %d %d %d %d %15s %d %d %d %d", x->name, x->file, &nf, &fw, &fh, &ow, &oh, &typ, &tf, fmt, &r, &g, &b, &sn) != 14) continue;
        x->frames = nf; x->srcframes = sn > 255 ? 255 : (sn < nf ? nf : sn); x->fw = fw; x->fh = fh; x->ow = ow; x->oh = oh; x->type = typ;
        x->rgba = !strcmp(fmt, "RGBA32");
        x->tint[0] = r; x->tint[1] = g; x->tint[2] = b;
        x->file_id = -1;
        for (int k = 0; k < num_fxfile; k++) if (!strcmp(fxfile[k].name, x->file)) x->file_id = k;
        if (x->file_id < 0 && num_fxfile < MAX_FXFILE) { strcpy(fxfile[num_fxfile].name, x->file); x->file_id = num_fxfile++; }
        num_fxspr++;
    }
    fclose(f);
}
int fxspr_find(const char *name)
{
    for (int i = 0; i < num_fxspr; i++) if (!strcasecmp(fxspr[i].name, name)) return fxspr[i].file_id >= 0 ? i : -1;
    return -1;
}
// before a map is loaded: give the memory back while the heap is being emptied anyway
void fxspr_free_all(void)
{
    for (int k = 0; k < num_fxfile; k++) {
        if (fxfile[k].sp) { sprite_free(fxfile[k].sp); fxfile[k].sp = NULL; }
        fxfile[k].failed = false;
    }
    memset(spr_col, 0, sizeof spr_col);
    memset(spr_vis, 0, sizeof spr_vis);
}
static sprite_t *fxspr_sprite(const fxspr_t *x)
{
    int k = x->file_id;
    if (k < 0) return NULL;
    if (fxfile[k].sp || fxfile[k].failed) return fxfile[k].sp;
    heap_stats_t hs;
    sys_get_heap_stats(&hs);
    int need = x->fw * x->fh * x->frames * (x->rgba ? 4 : 1);
    if (hs.total - hs.used < need + 160 * 1024) { fxfile[k].failed = true; debugf("fxspr %s: no room (%d KB)\n", x->file, need / 1024); return NULL; }
    char path[48];
    snprintf(path, sizeof path, "rom:/spr/%s.sprite", x->file);
    fxfile[k].sp = sprite_load(path);
    if (!fxfile[k].sp) fxfile[k].failed = true;
    return fxfile[k].sp;
}
static sprite_t *fx_flare;
// one quad (or several 30-texel tiles of it: TMEM holds 32x32 RGBA32) between four projected corners
static void quad_tiles(sprite_t *sp, const fxspr_t *x, int frame, float c[4][3])
{
    surface_t surf = sprite_get_pixels(sp);
    int y0 = frame * x->fh;
    int step = x->rgba ? 30 : 64;
    for (int ty = 0; ty < x->fh; ty += step)
        for (int tx = 0; tx < x->fw; tx += step) {
            int s0 = tx, s1 = tx + step < x->fw ? tx + step : x->fw;
            int t0 = ty, t1 = ty + step < x->fh ? ty + step : x->fh;
            // a texel of overlap so bilinear filtering doesn't seam the tiles
            int us0 = s0 > 0 ? s0 - 1 : 0, us1 = s1 < x->fw ? s1 + 1 : s1;
            int ut0 = t0 > 0 ? t0 - 1 : 0, ut1 = t1 < x->fh ? t1 + 1 : t1;
            rdpq_sync_tile(); rdpq_sync_load();
            rdpq_tex_upload_sub(TILE0, &surf, &(rdpq_texparms_t){.s.repeats = 1, .t.repeats = 1}, us0, y0 + ut0, us1, y0 + ut1);
            float v[4][6];
            const int ss[4] = {s0, s1, s0, s1}, tt[4] = {t0, t0, t1, t1};
            for (int q = 0; q < 4; q++) {
                float u = (float)ss[q] / x->fw, w = (float)tt[q] / x->fh;
                // corners: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
                for (int k = 0; k < 3; k++)
                    v[q][k] = (c[0][k] * (1 - u) + c[1][k] * u) * (1 - w) + (c[2][k] * (1 - u) + c[3][k] * u) * w;
                v[q][3] = ss[q]; v[q][4] = y0 + tt[q]; v[q][5] = 1;
            }
            rdpq_triangle(&TRIFMT_ZBUF_TEX, v[0], v[1], v[2]);
            rdpq_triangle(&TRIFMT_ZBUF_TEX, v[1], v[3], v[2]);
        }
}
static float fx_right[3] = {0, -1, 0}, fx_fwd[3] = {1, 0, 0};
// a camera-facing PC sprite at o, half size hw world units, animated at fps (projectiles)
static bool fxspr_billboard(int id, const float *o, float hw, float t, float fps, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (id < 0) return false;
    const fxspr_t *x = &fxspr[id];
    sprite_t *sp = fxspr_sprite(x);
    if (!sp) return false;
    float d[3] = {o[0] - fx_eye[0], o[1] - fx_eye[1], o[2] - fx_eye[2]};
    float dep = d[0] * fx_fwd[0] + d[1] * fx_fwd[1] + d[2] * fx_fwd[2];
    float cx, cy;
    if (dep < 8 || !project(o, &cx, &cy)) return true;
    float px = fminf(hw * 160 / dep, 140), py = px * x->oh / (x->ow ? x->ow : 1);
    if (px < 1) return true;
    float c[4][3];
    for (int q = 0; q < 4; q++) { c[q][0] = cx + ((q & 1) ? px : -px); c[q][1] = cy + ((q & 2) ? py : -py); c[q][2] = proj_z; }
    int frame = x->frames > 1 ? (int)(t * fps) % x->srcframes * x->frames / x->srcframes : 0;
    rdpq_sync_pipe(); rdpq_sync_tile(); rdpq_sync_load();
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (TEX0, 0, PRIM, 0)));
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_zbuf(true, false);
    rdpq_mode_filter(FILTER_BILINEAR);
    rdpq_set_prim_color(RGBA32(r * x->tint[0] / 255, g * x->tint[1] / 255, b * x->tint[2] / 255, a));
    quad_tiles(sp, x, frame, c);
    return true;
}
static void draw_env_sprites(void)
{
    extern int ents_sprites(int *out, int max);
    extern bool render_leaf_visible(int leaf);
    extern float ents_time(void);
    int list[96];
    int n = ents_sprites(list, 96), drawn = 0;
    float now = ents_time();
    int mode = 0;
    static unsigned framecnt;
    framecnt++;
    for (int i = 0; i < n && drawn < 24; i++) {
        int ei = list[i];
        entity_t *e = &entities[ei];
        float dx = e->origin[0] - fx_eye[0], dy = e->origin[1] - fx_eye[1], dz = e->origin[2] - fx_eye[2];
        if (dx * dx + dy * dy + dz * dz > 2000.0f * 2000.0f) continue;
        if (e->leaf == 0) e->leaf = bsp_leaf_for_point(e->origin);
        if (!render_leaf_visible(e->leaf)) continue;
        float sc = e->ltime > 0 ? e->ltime : (e->ltime = fmaxf(ent_value(e, "scale") ? atof(ent_value(e, "scale")) : 1.0f, 0.05f));
        if (!spr_col[ei]) {
            int r = 0, g = 0, b = 0;
            const char *rc = ent_value(e, "rendercolor");
            if (rc) sscanf(rc, "%d %d %d", &r, &g, &b);
            spr_col[ei] = 0x80000000u | (r & 255) << 16 | (g & 255) << 8 | (b & 255);
            const char *fr = ent_value(e, "framerate");
            spr_fr[ei] = fr ? atof(fr) : 10;
            if (spr_fr[ei] <= 0) spr_fr[ei] = 10;
        }
        const fxspr_t *x = e->sprid >= 0 ? &fxspr[e->sprid] : NULL;
        sprite_t *sp = x ? fxspr_sprite(x) : NULL;
        int frame = 0;
        if (x && x->frames > 1) {
            float t = now - e->sprt0;
            if (t < 0) t = 0;
            // the strip may hold fewer frames than the .spr: time runs over the .spr's frames
            int total = x->srcframes;
            frame = (int)(t * spr_fr[ei]);
            if (e->code == 5 && frame >= total) { e->active = false; continue; }
            frame = frame % total * x->frames / total;
        } else if (x && e->code == 5 && now - e->sprt0 > 0.3f) { e->active = false; continue; }
        // colour: glow / colour modes use rendercolor; additive art keeps its own colours
        int rm = e->rendermode;
        uint32_t col = spr_col[ei];
        uint8_t cr = 255, cg = 255, cb = 255;
        if (x) { cr = x->tint[0]; cg = x->tint[1]; cb = x->tint[2]; }
        if ((rm == 1 || rm == 3 || !x || (x && strcmp(x->name, x->file))) && (col & 0xFFFFFF)) {
            cr = col >> 16 & 255; cg = col >> 8 & 255; cb = col & 255;
        }
        int a = rm == 0 ? 255 : (e->renderamt > 0 ? e->renderamt : 255);
        float gsc = sc;
        if (rm == 3 && e->renderfx != 14) {
            // R_GlowBlend (kRenderGlow without kRenderFxNoDissipation): the same size on screen at
            // any distance (scale = dist * 0.005), brightness 19000 / dist^2 between 0.05 and 1
            float d2 = dx * dx + dy * dy + dz * dz, dist = sqrtf(d2);
            gsc = dist * 0.005f;
            float br = d2 > 1 ? 19000.0f / d2 : 1;
            a = (int)(a * fminf(fmaxf(br, 0.05f), 1.0f));
            if (a < 4) continue;
        }
        // size: the sprite's own pixels are world units, times "scale"
        float hw, hh;
        if (x) { hw = x->ow * 0.5f * gsc; hh = x->oh * 0.5f * gsc; }
        else { hw = hh = (e->code == 3 ? 40 : 14) * gsc; sp = e->code == 3 ? fx_projspr[4] : fx_flare; }
        if (!sp) continue;
        // corners: facing the camera (types 0-2, 4), or in the entity's own plane (3: ORIENTED)
        float rv[3], uv[3];
        if (x && x->type == 3) {
            float p = e->angles[0] * (float)M_PI / 180, y = e->angles[1] * (float)M_PI / 180, ro = e->angles[2] * (float)M_PI / 180;
            float sp_ = sinf(p), cp = cosf(p), sy = sinf(y), cy = cosf(y), sr = sinf(ro), cr_ = cosf(ro);
            rv[0] = -sr * sp_ * cy + cr_ * sy; rv[1] = -sr * sp_ * sy - cr_ * cy; rv[2] = -sr * cp;
            uv[0] = cr_ * sp_ * cy + sr * sy; uv[1] = cr_ * sp_ * sy - sr * cy; uv[2] = cr_ * cp;
            for (int k = 0; k < 3; k++) rv[k] = -rv[k];
        } else {
            memcpy(rv, fx_right, 12); memcpy(uv, fx_up, 12);
        }
        // glows (kRenderGlow) aren't depth tested on the PC: a trace from the eye decides whether they
        // show, so the light fixture they sit in doesn't cut them in half but a wall in front hides them
        bool glow = rm == 3;
        if (glow) {
            if (!spr_vis[ei] || ((framecnt + ei) & 3) == 0) {
                gtrace_t tr;
                game_trace(fx_eye, e->origin, NULL, NULL, TR_IGNORE_MONSTERS | TR_IGNORE_PLAYER, -1, &tr);
                // R_GlowBlend: hidden only when more than 8 units of the line are blocked
                float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                spr_vis[ei] = (1 - tr.fraction) * dist <= 8 ? 1 : 2;
            }
#ifdef SPRDEBUG
            { static float nt2; if (now > nt2 && dx * dx + dy * dy < 400 * 400) { nt2 = now + 1; debugf("SPRV %d vis %d d %.0f\n", ei, spr_vis[ei], sqrtf(dx * dx + dy * dy + dz * dz)); } }
#endif
            if (spr_vis[ei] != 1) continue;
        }
        float c[4][3];
        bool ok = true;
        if (x && x->type == 3) {   // in its own plane: project each corner
            for (int q = 0; q < 4 && ok; q++) {
                float sx = (q & 1) ? hw : -hw, sy = (q & 2) ? -hh : hh;
                float w[3];
                for (int k = 0; k < 3; k++) w[k] = e->origin[k] + rv[k] * sx + uv[k] * sy;
                ok = project(w, &c[q][0], &c[q][1]);
                c[q][2] = proj_z;
            }
        } else {
            // facing the camera: one projection, the corners are the centre +- the size / depth
            // (90 degree FOV on 320x240: 160 pixels per unit of x/depth and of y/depth)
            float cx, cy;
            float dep = dx * fx_fwd[0] + dy * fx_fwd[1] + dz * fx_fwd[2];
            if (dep < 8 || !project(e->origin, &cx, &cy)) continue;
            float px = hw * 160 / dep, py = hh * 160 / dep;
            for (int q = 0; q < 4; q++) {
                c[q][0] = cx + ((q & 1) ? px : -px);
                c[q][1] = cy + ((q & 2) ? py : -py);
                c[q][2] = proj_z;
            }
        }
        if (!ok) continue;
        float ext = fmaxf(fabsf(c[3][0] - c[0][0]), fabsf(c[3][1] - c[0][1]));
        if (ext < 1.5f) continue;
        if (fmaxf(fmaxf(c[0][0], c[1][0]), fmaxf(c[2][0], c[3][0])) < 0 || fminf(fminf(c[0][0], c[1][0]), fminf(c[2][0], c[3][0])) > 320) continue;
        if (fmaxf(fmaxf(c[0][1], c[1][1]), fmaxf(c[2][1], c[3][1])) < 0 || fminf(fminf(c[0][1], c[1][1]), fminf(c[2][1], c[3][1])) > 240) continue;
        if (mode != 1 + glow) {
            rdpq_sync_pipe(); rdpq_sync_tile(); rdpq_sync_load();
            rdpq_set_mode_standard();
            rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (TEX0, 0, PRIM, 0)));
            rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
            rdpq_mode_zbuf(!glow, false);
            rdpq_mode_filter(FILTER_BILINEAR);
            mode = 1 + glow;
        }
#ifdef SPRDEBUG
        { static float nt; if (now > nt) { nt = now + 1; debugf("SPR fmt %d %dx%d | %d %s fr %d sc %.2f hw %.0f c0 %.0f %.0f %.3f c3 %.0f %.0f %.3f a %d\n", sp ? (int)sprite_get_format(sp) : -1, sp ? sp->width : 0, sp ? sp->height : 0, ei, x ? x->name : "-", frame, sc, hw, c[0][0], c[0][1], c[0][2], c[3][0], c[3][1], c[3][2], a); } }
#endif
        rdpq_set_prim_color(RGBA32(cr, cg, cb, a));
        if (x) quad_tiles(sp, x, frame, c);
        else {
            rdpq_sync_tile(); rdpq_sync_load();
            rdpq_sprite_upload(TILE0, sp, NULL);
            float tw = sp->width, th = sp->height;
            float v0[6] = {c[0][0], c[0][1], c[0][2], 0, 0, 1}, v1[6] = {c[1][0], c[1][1], c[1][2], tw, 0, 1};
            float v2[6] = {c[2][0], c[2][1], c[2][2], 0, th, 1}, v3[6] = {c[3][0], c[3][1], c[3][2], tw, th, 1};
            rdpq_triangle(&TRIFMT_ZBUF_TEX, v0, v1, v2);
            rdpq_triangle(&TRIFMT_ZBUF_TEX, v1, v3, v2);
        }
        drawn++;
    }
}
static void draw_bills(bool view)
{
    if (!view) draw_env_sprites();
    // model-less projectiles as glowing billboards
    for (int i = 0; !view && i < MAX_PROJ; i++) {
        proj_t *p = &projs[i];
        if (p->kind == P_NONE || p->model) continue;
        int si; float rad; uint8_t r = 255, g = 255, b = 255;
        switch (p->kind) {
        case P_SPIT: si = 0; rad = 6; r = 170; g = 230; b = 60; break;
        case P_MORTAR: si = 1; rad = 14; r = 160; g = 255; b = 120; break;
        case P_NIHZAP: si = 2; rad = 14; break;
        case P_BALL: si = 3; rad = 10; break;
        case P_TELEBALL: si = 4; rad = 24; break;
        default: continue;
        }
        {   // the PC's own animated art where we have it (nihilanth.cpp / controller.cpp)
            static int ids[3] = {-2, -2, -2};
            if (ids[0] == -2) { ids[0] = fxspr_find("nhth1"); ids[1] = fxspr_find("xspark4"); ids[2] = fxspr_find("exit1"); }
            int id = si == 2 ? ids[0] : si == 3 ? ids[1] : si == 4 ? ids[2] : -1;
            float hw = si == 4 ? 40 : si == 2 ? 16 : 12;   // exit1 is 80 units across: the teleport ball
            if (id >= 0 && fxspr_billboard(id, p->o, hw, wtime + i * 0.37f, si == 4 ? 20 : 15, r, g, b, 255)) continue;
        }
        sprite_t *sp = fx_projspr[si];
        if (!sp) continue;
        float x, y, x2, y2;
        if (!project(p->o, &x, &y)) continue;
        float z = proj_z;
        float up3[3] = {p->o[0] + fx_up[0] * rad, p->o[1] + fx_up[1] * rad, p->o[2] + fx_up[2] * rad};
        if (!project(up3, &x2, &y2)) continue;
        float rr = sqrtf((x2 - x) * (x2 - x) + (y2 - y) * (y2 - y));
        if (rr < 2) rr = 2;
        if (rr > 120) rr = 120;
        rdpq_sync_pipe(); rdpq_sync_tile(); rdpq_sync_load();
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (TEX0, 0, PRIM, 0)));
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_zbuf(true, false);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_sprite_upload(TILE0, sp, NULL);
        rdpq_set_prim_color(RGBA32(r, g, b, 255));
        float tw = sp->width, th = sp->height;
        float v0[6] = {x - rr, y - rr, z, 0, 0, 1}, v1[6] = {x + rr, y - rr, z, tw, 0, 1};
        float v2[6] = {x - rr, y + rr, z, 0, th, 1}, v3[6] = {x + rr, y + rr, z, tw, th, 1};
        rdpq_triangle(&TRIFMT_ZBUF_TEX, v0, v1, v2);
        rdpq_triangle(&TRIFMT_ZBUF_TEX, v1, v3, v2);
    }
    for (int i = 0; i < MAX_BILLS; i++) {
        bill_t *bl = &bills[i];
        float age = wtime - bl->start;
        if (bl->life <= 0 || age < 0 || age > bl->life || bl->view != view) continue;
        if (view && !vm_att_ok) continue;
        sprite_t *sp = bl->kind == 0 ? fx_expl[(int)(age / bl->life * 8) & 7]
                     : bl->kind == 4 ? fx_xspark[(int)(wtime * 15) % 11] : fx_muzzle[(bl->kind - 1) % 3];
        if (!sp) continue;
        const float *o = view ? vm_att[0] : bl->o;
        float x, y, x2, y2;
        if (!project(o, &x, &y)) continue;
        float z = proj_z;
        float up3[3] = {o[0] + fx_up[0] * bl->rad, o[1] + fx_up[1] * bl->rad, o[2] + fx_up[2] * bl->rad};
        if (!project(up3, &x2, &y2)) continue;
        float rad = sqrtf((x2 - x) * (x2 - x) + (y2 - y) * (y2 - y));
        if (rad < 2) rad = 2;
        if (rad > 160) rad = 160;
        rdpq_sync_pipe(); rdpq_sync_tile(); rdpq_sync_load();
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (TEX0, 0, PRIM, 0)));
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_zbuf(!view, false);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_sprite_upload(TILE0, sp, NULL);
        float fade = bl->kind == 0 ? (age < bl->life * 0.6f ? 1 : 1 - (age - bl->life * 0.6f) / (bl->life * 0.4f)) : 1;
        rdpq_set_prim_color(RGBA32(bl->r, bl->g, bl->b, (int)(255 * fade)));
        float tw = sp->width, th = sp->height;
        float v0[6] = {x - rad, y - rad, z, 0, 0, 1}, v1[6] = {x + rad, y - rad, z, tw, 0, 1};
        float v2[6] = {x - rad, y + rad, z, 0, th, 1}, v3[6] = {x + rad, y + rad, z, tw, th, 1};
        rdpq_triangle(&TRIFMT_ZBUF_TEX, v0, v1, v2);
        rdpq_triangle(&TRIFMT_ZBUF_TEX, v1, v3, v2);
    }
}


static float fl_point[3];
static bool fl_valid;
// where the flashlight's dynamic light is this frame (for lighting characters)
bool weapons_flashlight_point(float *out) { if (fl_valid) memcpy(out, fl_point, 12); return fl_valid; }
void weapons_draw3d(const float *eye, const float *angles, const uint8_t *light)
{
    memcpy(fx_eye, eye, 12); memcpy(fx_angles, angles, 12);
    vm_att_ok = false;
    {
        float sp_ = sinf(angles[0] * (float)M_PI / 180), cp_ = cosf(angles[0] * (float)M_PI / 180);
        float sy_ = sinf(angles[1] * (float)M_PI / 180), cy_ = cosf(angles[1] * (float)M_PI / 180);
        fx_up[0] = sp_ * cy_; fx_up[1] = sp_ * sy_; fx_up[2] = cp_;
        fx_right[0] = sy_; fx_right[1] = -cy_; fx_right[2] = 0;
        fx_fwd[0] = cp_ * cy_; fx_fwd[1] = cp_ * sy_; fx_fwd[2] = -sp_;
    }
    for (int i = 0; i < MAX_PROJ; i++) {
        proj_t *p = &projs[i];
        if (p->kind == P_NONE || !p->model) continue;
        studio_draw_t d = {.model = p->model, .yaw = p->yaw, .seq = 0, .frame = p->frame, .body = p->kind == P_TRIPMINE ? 3 : 0};
        if (p->kind == P_TRIPMINE) { int s = studio_find_seq(p->model, "world"); d.seq = s >= 0 ? s : 0; }
        memcpy(d.origin, p->o, 12);
        bsp_light_at(p->o, d.light);
        studio_draw(&d);
    }
    for (int i = 0; i < MAX_GIBS; i++) {
        gib_t *g = &gibs[i];
        if (!g->m || g->end < wtime) continue;
        studio_draw_t d = {.model = g->m, .yaw = g->yaw, .seq = 0, .frame = 0, .body = g->body};
        memcpy(d.origin, g->o, 12);
        bsp_light_at(g->o, d.light);
        studio_draw(&d);
    }
    // particles / beams: drawn while the world depth is still in the z-buffer so walls hide them
#ifdef PROFX
    uint32_t wt0 = TICKS_READ();
#endif
    weapons_draw_effects();
    { extern void render_draw_translucent(void); render_draw_translucent(); }
#ifdef PROFX
    uint32_t wt1 = TICKS_READ();
    { pacc[0] += TICKS_DISTANCE(wt0, pst[0]); pacc[1] += TICKS_DISTANCE(pst[0], pst[1]); pacc[2] += TICKS_DISTANCE(pst[1], pst[2]); pacc[3] += TICKS_DISTANCE(pst[2], pst[3]);
      if (++pn_ == 30) { debugf("  fx parts: flash %d tex %d bills %d parts %d\n", (int)TICKS_TO_US(pacc[0]/30), (int)TICKS_TO_US(pacc[1]/30), (int)TICKS_TO_US(pacc[2]/30), (int)TICKS_TO_US(pacc[3]/30)); memset(pacc, 0, sizeof(pacc)); pn_ = 0; } }
    rspq_flush();
    uint32_t wt2 = TICKS_READ();
    { static uint32_t a, b; static int n; a += TICKS_DISTANCE(wt0, wt1); b += TICKS_DISTANCE(wt1, wt2);
      if (++n == 30) { debugf("  wdraw effects %d us, flush %d us\n", (int)TICKS_TO_US(a / n), (int)TICKS_TO_US(b / n)); a = b = n = 0; } }
#endif
    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    rdpq_mode_begin();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_antialias(AA_NONE);
    rdpq_mode_zbuf(true, true);
    rdpq_mode_end();
#ifdef NOVIEWMODEL
    return;
#endif
    if (!vmodel || !player_alive() || cur == 0 || weapons_hidden) return;
    // view model: at the eye, turned with the view (V_CalcRefdef), bobbing a little
    float sp = sinf(angles[0] * (float)M_PI / 180), cp = cosf(angles[0] * (float)M_PI / 180);
    float sy = sinf(angles[1] * (float)M_PI / 180), cy = cosf(angles[1] * (float)M_PI / 180);
    float sr = sinf(angles[2] * (float)M_PI / 180), cr = cosf(angles[2] * (float)M_PI / 180);
    float f[3] = {cp * cy, cp * sy, -sp};
    float r[3] = {-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp};
    float u[3] = {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
    float v[3];
    pm_get_velocity(v);
    float speed = sqrtf(v[0] * v[0] + v[1] * v[1]);
    float bob = sinf(bob_t * 7.8f) * fminf(speed / 320.0f, 1.0f) * 0.6f;
    studio_draw_t d = {.model = vmodel, .seq = vseq, .frame = vframe, .body = vbody, .use_xform = true, .att_out = vm_att, .att_n = 1};
    for (int k = 0; k < 3; k++) {
        d.xform[k][0] = f[k];
        d.xform[k][1] = -r[k];
        d.xform[k][2] = u[k];
        d.xform[k][3] = eye[k] + f[k] * bob * 0.4f + u[k] * (bob * 0.3f - 1);
    }
    memcpy(d.light, light, 3);
    d.light[0] = d.light[0] < 60 ? 60 : d.light[0];
    d.light[1] = d.light[1] < 60 ? 60 : d.light[1];
    d.light[2] = d.light[2] < 60 ? 60 : d.light[2];
#ifndef NOCLEARZ
    t3d_screen_clear_depth();
#endif
    studio_draw(&d);
    vm_att_ok = vmodel->numatt > 0;
    // the gun's own muzzle flash over the view model (no depth test, like the PC's)
    bool anyv = false;
    for (int i = 0; i < MAX_BILLS; i++) if (bills[i].view && wtime - bills[i].start <= bills[i].life) anyv = true;
    if (anyv) {
        rdpq_sync_pipe(); rdpq_sync_tile(); rdpq_sync_load();
        draw_bills(true);
        rdpq_set_mode_standard();
    }
}

// beams and explosion flashes as 2D overlays (no sprite renderer)
void weapons_draw_effects(void)
{
    // the RSP (tiny3d) has been drawing: rdpq's automatic syncs don't see that, so changing the
    // RDP mode or TMEM now without these hangs the real RDP (fine in emulators)
#ifdef PROFX
    uint32_t qa = TICKS_READ();
#endif
    rdpq_sync_pipe();
    rdpq_sync_tile();
    rdpq_sync_load();
#ifdef PROFX
    { static uint32_t acc; static int n; acc += TICKS_DISTANCE(qa, TICKS_READ()); if (++n == 30) { debugf("  syncs %d us\n", (int)TICKS_TO_US(acc / n)); acc = n = 0; } }
#endif
    // flashlight (GoldSrc EF_DIMLIGHT on the local player): a dynamic light placed where the view
    // trace hits, radius ~100 units, full brightness at its centre falling off to nothing at the edge.
    // Surfaces get an additive, smoothly shaded disc; characters inside the radius are lit (render.c).
    fl_valid = false;
    if (player.flashlight && player_alive()) {
        float p = fx_angles[0] * (float)M_PI / 180, y = fx_angles[1] * (float)M_PI / 180;
        float f[3] = {cosf(p) * cosf(y), cosf(p) * sinf(y), -sinf(p)};
        float end[3] = {fx_eye[0] + f[0] * 2048, fx_eye[1] + f[1] * 2048, fx_eye[2] + f[2] * 2048};
        gtrace_t tr;
        game_trace(fx_eye, end, NULL, NULL, TR_IGNORE_PLAYER, -1, &tr);
        if (tr.fraction < 1) {
            memcpy(fl_point, tr.endpos, 12);
            fl_valid = true;
            float n[3] = {tr.normal[0], tr.normal[1], tr.normal[2]};
            float a[3] = {fabsf(n[2]) < 0.9f ? 0 : 1, 0, fabsf(n[2]) < 0.9f ? 1 : 0}, u[3], v[3];
            u[0] = n[1] * a[2] - n[2] * a[1]; u[1] = n[2] * a[0] - n[0] * a[2]; u[2] = n[0] * a[1] - n[1] * a[0];
            float ul = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]); if (ul < 1e-4f) ul = 1;
            for (int k = 0; k < 3; k++) u[k] /= ul;
            v[0] = n[1] * u[2] - n[2] * u[1]; v[1] = n[2] * u[0] - n[0] * u[2]; v[2] = n[0] * u[1] - n[1] * u[0];
            float c[3];
            for (int k = 0; k < 3; k++) c[k] = tr.endpos[k] + n[k] * 1.0f;
            // light hitting a surface at a glancing angle is spread thinner
            float ndl = -(n[0] * f[0] + n[1] * f[1] + n[2] * f[2]);
            if (ndl < 0.2f) ndl = 0.2f;
            int lv = (int)(100 * ndl);
            float cx, cy;
            if (project(c, &cx, &cy)) {
                float cz = proj_z;
                rdpq_set_mode_standard();
                rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
                rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
                rdpq_mode_zbuf(true, false);
                const float R = 100;
                float prev[7]; bool havep = false;
                for (int k = 0; k <= 16; k++) {
                    float t = k * (float)M_PI / 8, w[3], sx, sy;
                    for (int q = 0; q < 3; q++) w[q] = c[q] + (u[q] * cosf(t) + v[q] * sinf(t)) * R;
                    if (!project(w, &sx, &sy)) { havep = false; continue; }
                    float cur[7] = {sx, sy, proj_z, 1, 1, 0.9f, 0};
                    float cc[7] = {cx, cy, cz, 1, 1, 0.9f, lv / 255.0f};
                    if (havep) rdpq_triangle(&TRIFMT_ZBUF_SHADE, cc, prev, cur);
                    memcpy(prev, cur, sizeof cur); havep = true;
                }
                rdpq_set_mode_standard();
            }
        }
    }
    PSTAMP(0);
    // textured effects: beams with lgtning/laserbeam.spr, impact puffs and blood
    {

        for (int tex = 0; tex < 5; tex++) {
            sprite_t *sp = fx_spr[tex];
            if (!sp) continue;
            bool up = false;
            float tw = sp->width, th = sp->height;
            if (tex < 2 || tex == 4) {
                for (int i = 0; i < MAX_BEAMS; i++) {
                    beam_t *b = &beams[i];
                    if (b->end < wtime || b->tex != tex + 1) continue;
                    float ax, ay, bx, by;
                    if (!project(b->a, &ax, &ay)) continue;
                    float za = proj_z;
                    if (!project(b->b, &bx, &by)) continue;
                    float zb = proj_z;
                    float dx = bx - ax, dy = by - ay, l = sqrtf(dx * dx + dy * dy);
                    if (l < 0.5f) continue;
                    if (!up) {
                        rdpq_set_mode_standard();
                        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (TEX0, 0, PRIM, 0)));
                        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
                        rdpq_mode_zbuf(true, false);
                        rdpq_sync_tile(); rdpq_sync_load(); rdpq_sprite_upload(TILE0, sp, &(rdpq_texparms_t){.s.repeats = REPEAT_INFINITE, .t.repeats = REPEAT_INFINITE});
                        up = true;
                    }
                    float a = (b->end - wtime) / b->life;
                    rdpq_set_prim_color(RGBA32(b->r, b->g, b->bl, (int)(255 * a)));
                    // half widths at both ends: pixels, or world units seen in perspective
                    float wa = b->width * 0.5f, wb = wa;
                    if (b->world_w) {
                        float ux, uy;
                        float pa[3] = {b->a[0] + fx_up[0] * b->width * 0.5f, b->a[1] + fx_up[1] * b->width * 0.5f, b->a[2] + fx_up[2] * b->width * 0.5f};
                        float pb[3] = {b->b[0] + fx_up[0] * b->width * 0.5f, b->b[1] + fx_up[1] * b->width * 0.5f, b->b[2] + fx_up[2] * b->width * 0.5f};
                        wa = project(pa, &ux, &uy) ? fminf(sqrtf((ux - ax) * (ux - ax) + (uy - ay) * (uy - ay)), 60) : 1;
                        wb = project(pb, &ux, &uy) ? fminf(sqrtf((ux - bx) * (ux - bx) + (uy - by) * (uy - by)), 60) : 1;
                        if (wa < 0.75f) wa = 0.75f;
                        if (wb < 0.75f) wb = 0.75f;
                    }
                    float nx = -dy / l, ny = dx / l;
                    // GoldSrc beams: the sprite's width runs across the beam, its height along it
                    // r_beams R_DrawSegs: the sprite's width runs across the beam and its height
                    // along it, once every 100 units (length * 0.01, at least half the sprite);
                    // the egon's scrolls (SetScrollRate), lightning starts at a random place
                    float wl = sqrtf((b->b[0] - b->a[0]) * (b->b[0] - b->a[0]) + (b->b[1] - b->a[1]) * (b->b[1] - b->a[1]) + (b->b[2] - b->a[2]) * (b->b[2] - b->a[2]));
                    float span = fminf(fmaxf(wl * 0.01f, 0.5f) * th, 900.0f);   // texture coordinates stay inside s10.5
                    float t0 = b->world_w ? fmodf(wtime * 110, th) : (float)(rand() % (int)th);
                    float t1 = t0 + span;
                    float v0[6] = {ax + nx * wa, ay + ny * wa, za, 0, t0, 1}, v1[6] = {ax - nx * wa, ay - ny * wa, za, tw, t0, 1};
                    float v2[6] = {bx + nx * wb, by + ny * wb, zb, 0, t1, 1}, v3[6] = {bx - nx * wb, by - ny * wb, zb, tw, t1, 1};
                    rdpq_triangle(&TRIFMT_ZBUF_TEX, v0, v1, v2);
                    rdpq_triangle(&TRIFMT_ZBUF_TEX, v1, v3, v2);
                }
            } else {
                for (int i = 0; i < MAX_PARTS; i++) {
                    part_t *p = &parts[i];
                    if (p->end < wtime || p->kind != tex - 1) continue;
                    float x, y, x2, y2;
                    if (!project(p->o, &x, &y)) continue;
                    float z = proj_z;
                    float age = 1 - (p->end - wtime) / p->life;
                    float up3[3] = {p->o[0], p->o[1], p->o[2] + 6 + age * 10};
                    if (!project(up3, &x2, &y2)) continue;
                    float rad = fabsf(y2 - y);
                    if (rad < 1) rad = 1;
                    if (rad > 40) rad = 40;
                    if (!up) {
                        rdpq_set_mode_standard();
                        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (TEX0, 0, PRIM, 0)));
                        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
                        rdpq_mode_zbuf(true, false);
                        rdpq_sync_tile(); rdpq_sync_load(); rdpq_sprite_upload(TILE0, sp, NULL);
                        up = true;
                    }
                    rdpq_set_prim_color(RGBA32(p->r, p->g, p->b, (int)(255 * (1 - age))));
                    float v0[6] = {x - rad, y - rad, z, 0, 0, 1}, v1[6] = {x + rad, y - rad, z, tw, 0, 1};
                    float v2[6] = {x - rad, y + rad, z, 0, th, 1}, v3[6] = {x + rad, y + rad, z, tw, th, 1};
                    rdpq_triangle(&TRIFMT_ZBUF_TEX, v0, v1, v2);
                    rdpq_triangle(&TRIFMT_ZBUF_TEX, v1, v3, v2);
                }
            }
        }
        PSTAMP(1);
        draw_bills(false);
        rdpq_set_mode_standard();
    }
    PSTAMP(2);
    // particles
    {
        bool anyp = false;
        float dt = 1.0f / 30;
        for (int i = 0; i < MAX_PARTS; i++) {
            part_t *p = &parts[i];
            if (p->end < wtime) continue;
            for (int k = 0; k < 3; k++) p->o[k] += p->v[k] * dt;
            p->v[2] -= p->grav * dt;
            float x, y;
            if (p->kind) continue;
            if (!project(p->o, &x, &y)) continue;
            if (x < 0 || y < 0 || x > 318 || y > 238) continue;
            if (!anyp) {
                // 2x2 rectangles at a fixed depth (primitive Z) instead of two CPU-set-up triangles each
                rdpq_set_mode_standard(); rdpq_mode_combiner(RDPQ_COMBINER_FLAT); rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
                rdpq_mode_zbuf(true, false); rdpq_mode_zoverride(true, 0, 0);
                anyp = true;
            }
            float a = (p->end - wtime) / p->life;
            rdpq_set_prim_depth_raw((uint16_t)(proj_z * 0x7FFF), 0);
            rdpq_set_prim_color(RGBA32(p->r, p->g, p->b, (int)(255 * a)));
            rdpq_fill_rectangle(x, y, x + 2, y + 2);
        }
        if (anyp) rdpq_set_mode_standard();
    }
    PSTAMP(3);
    // shock rings: wall segments around the centre, the laser beam's soft profile across the height
    {
        bool mode = false;
        sprite_t *sp = fx_spr[1];
        for (int i = 0; i < MAX_RINGS; i++) {
            ring_t *rg = &rings[i];
            float age = wtime - rg->start;
            if (rg->life <= 0 || age < 0 || age > rg->life) continue;
            float f = age / rg->life, rad = rg->maxr * f;
#ifdef TESTRING
            { static float nd; if (wtime > nd) { nd = wtime + 0.2f; float x, y; bool ok = project(rg->o, &x, &y); debugf("RING %d age %.2f rad %.0f proj %d %.0f %.0f\n", i, age, rad, ok, x, y); } }
#endif
            if (rad < 4) continue;
            int alpha = (int)(255 * (1 - f * f));
            if (!mode) {
                rdpq_sync_pipe(); rdpq_sync_tile(); rdpq_sync_load();
                rdpq_set_mode_standard();
                rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
                rdpq_mode_zbuf(true, false);
                if (sp) {
                    rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (TEX0, 0, PRIM, 0)));
                    rdpq_mode_filter(FILTER_BILINEAR);
                    rdpq_sprite_upload(TILE0, sp, &(rdpq_texparms_t){.s.repeats = REPEAT_INFINITE, .t.repeats = REPEAT_INFINITE});
                } else rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
                mode = true;
            }
            rdpq_set_prim_color(RGBA32(rg->r, rg->g, rg->b, alpha));
            const int N = 24;
            float tw = sp ? sp->width : 1, th = sp ? sp->height : 1;
            float pv[2][4];   // previous column: bottom x,y,z, top x,y,z (+valid)
            bool havep = false;
            for (int k = 0; k <= N; k++) {
                float a = k * (2 * (float)M_PI / N);
                float lo[3] = {rg->o[0] + cosf(a) * rad, rg->o[1] + sinf(a) * rad, rg->o[2]};
                float hi[3] = {lo[0], lo[1], lo[2] + rg->height};
                float bx, by, tx, ty;
                bool ok = project(lo, &bx, &by);
                float bz = proj_z;
                ok = ok && project(hi, &tx, &ty);
                float tz = proj_z;
                if (!ok) { havep = false; continue; }
                float tcoord = k * th * 0.5f;
                if (havep) {
                    float v0[6] = {pv[0][0], pv[0][1], pv[0][2], 0, pv[0][3], 1}, v1[6] = {pv[1][0], pv[1][1], pv[1][2], tw, pv[1][3], 1};
                    float v2[6] = {bx, by, bz, 0, tcoord, 1}, v3[6] = {tx, ty, tz, tw, tcoord, 1};
                    if (sp) { rdpq_triangle(&TRIFMT_ZBUF_TEX, v0, v1, v2); rdpq_triangle(&TRIFMT_ZBUF_TEX, v1, v3, v2); }
                    else { rdpq_triangle(&TRIFMT_ZBUF, v0, v1, v2); rdpq_triangle(&TRIFMT_ZBUF, v1, v3, v2); }
                }
                pv[0][0] = bx; pv[0][1] = by; pv[0][2] = bz; pv[0][3] = tcoord;
                pv[1][0] = tx; pv[1][1] = ty; pv[1][2] = tz; pv[1][3] = tcoord;
                havep = true;
            }
        }
        if (mode) rdpq_set_mode_standard();
    }
    bool any = false;
    for (int i = 0; i < MAX_BEAMS; i++) if (beams[i].end >= wtime) any = true;
    for (int i = 0; i < 8; i++) if (flashes[i].end >= wtime) any = true;
    // the RPG's laser dot
    bool dot = cur == WEAPON_RPG && rpg_laser && player_alive();
    if (!any && !dot) return;
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_zbuf(true, false);
    for (int i = 0; i < MAX_BEAMS; i++) {
        beam_t *b = &beams[i];
        if (b->end < wtime || (b->tex && fx_spr[b->tex - 1])) continue;
        float ax, ay, bx, by;
        if (!project(b->a, &ax, &ay)) continue;
        float za = proj_z;
        if (!project(b->b, &bx, &by)) continue;
        float zb = proj_z;
        float a = (b->end - wtime) / b->life;
        rdpq_set_prim_color(RGBA32(b->r, b->g, b->bl, (int)(255 * a)));
        float dx = bx - ax, dy = by - ay, l = sqrtf(dx * dx + dy * dy);
        if (l < 0.5f) continue;
        float nx = -dy / l * b->width * 0.5f, ny = dx / l * b->width * 0.5f;
        float v0[3] = {ax + nx, ay + ny, za}, v1[3] = {ax - nx, ay - ny, za}, v2[3] = {bx + nx, by + ny, zb}, v3[3] = {bx - nx, by - ny, zb};
        rdpq_triangle(&TRIFMT_ZBUF, v0, v1, v2);
        rdpq_triangle(&TRIFMT_ZBUF, v1, v3, v2);
    }
    for (int i = 0; i < 8; i++) {
        flash_t *fl = &flashes[i];
        if (fl->end < wtime) continue;
        float x, y;
        if (!project(fl->o, &x, &y)) continue;
        float a = (fl->end - wtime) / 0.35f;
        float eye[3];
        player_get_eye(eye);
        float d = sqrtf((eye[0] - fl->o[0]) * (eye[0] - fl->o[0]) + (eye[1] - fl->o[1]) * (eye[1] - fl->o[1]) + (eye[2] - fl->o[2]) * (eye[2] - fl->o[2]));
        float rad = fl->size * 160 / (d > 32 ? d : 32) * (1.2f - a * 0.5f);
        if (rad > 120) rad = 120;
        rdpq_set_prim_color(RGBA32(255, 180, 60, (int)(255 * a)));
        for (int k = 0; k < 8; k++) {
            float a0 = k * (float)M_PI / 4, a1 = (k + 1) * (float)M_PI / 4;
            float c[2] = {x, y}, p0[2] = {x + cosf(a0) * rad, y + sinf(a0) * rad}, p1[2] = {x + cosf(a1) * rad, y + sinf(a1) * rad};
            rdpq_triangle(&TRIFMT_FILL, c, p0, p1);
        }
    }
    if (dot) {
        float eye[3];
        player_get_eye(eye);
        float end[3] = {eye[0] + aim_dir[0] * 8192, eye[1] + aim_dir[1] * 8192, eye[2] + aim_dir[2] * 8192};
        gtrace_t tr;
        game_trace(eye, end, NULL, NULL, TR_IGNORE_PLAYER, -1, &tr);
        float x, y;
        if (project(tr.endpos, &x, &y)) {
            rdpq_set_prim_color(RGBA32(255, 30, 30, 255));
            rdpq_fill_rectangle(x - 1, y - 1, x + 2, y + 2);
        }
    }
    rdpq_set_mode_standard();
}

// ------------------------------------------------------------------ HUD (cl_dll/ammo.cpp)
static image_t hud1, hud2, xhair;
static bool hud_loaded;
static void hud_load(void);
void player_hud_init(void);
void weapons_hud_init(void)
{
    hud_load();
    player_hud_init();
    // effect sprites stay loaded: load them at boot, not in the middle of a map's heap
    const char *nm[4] = {"rom:/ui/fx_lgtning.sprite", "rom:/ui/fx_laserbeam.sprite", "rom:/ui/fx_wallpuff.sprite", "rom:/ui/fx_blood.sprite"};
    for (int k = 0; k < 4; k++) fx_spr[k] = sprite_load(nm[k]);
    fx_spr[4] = sprite_load("rom:/ui/fx_xbeam.sprite");
    char path[48];
    for (int k = 0; k < 8; k++) { snprintf(path, sizeof(path), "rom:/ui/fx_expl%d.sprite", k); fx_expl[k] = sprite_load(path); }
    for (int k = 0; k < 3; k++) { snprintf(path, sizeof(path), "rom:/ui/fx_muzzle%d.sprite", k + 1); fx_muzzle[k] = sprite_load(path); }
    for (int k = 0; k < 11; k++) { snprintf(path, sizeof(path), "rom:/ui/fx_xspark%d.sprite", k); fx_xspark[k] = sprite_load(path); }
    static const char *pn[5] = {"fx_spit", "fx_mspit", "fx_nball", "fx_cball", "fx_tball"};
    for (int k = 0; k < 5; k++) { snprintf(path, sizeof(path), "rom:/ui/%s.sprite", pn[k]); fx_projspr[k] = sprite_load(path); }
    fx_flare = sprite_load("rom:/ui/fx_flare.sprite");
    fxspr_load_index();
}
static float ammo_fade;
static int last_clip = -1, last_ammo = -1;


static void hud_load(void)
{
    if (hud_loaded) return;
    hud_loaded = true;
    image_load(&hud1, "rom:/ui/hud1.sprite");
    image_load(&hud2, "rom:/ui/hud2.sprite");
    image_load(&xhair, "rom:/ui/crosshairs.sprite");
}

// DrawHudNumber: 12x16 digits from 320hud2
int hud_number(int x, int y, int value, int digits, bool draw_zero)
{
    hud_load();
    char b[8];
    snprintf(b, sizeof(b), "%d", value);
    int n = strlen(b);
    for (int i = n; i < digits; i++) x += 12;
    for (int i = 0; i < n; i++) {
        int d = b[i] - '0';
        if (value == 0 && !draw_zero) { x += 12; continue; }
        image_draw(&hud2, x, y, d * 12, 0, 12, 16);
        x += 12;
    }
    return x;
}

void hud_icon2(int x, int y, int sx, int sy, int w, int h) { hud_load(); image_draw(&hud2, x, y, sx, sy, w, h); }

static color_t hud_color(float a)
{
    // RGB_YELLOWISH, ScaleColors(alpha)
    return RGBA32(255 * a / 255, 160 * a / 255, 0, 255);
}

void weapons_draw_hud(float dt, bool show_crosshair)
{
    hud_load();
    if (!player.suit) return;
    const wdef_t *w = wdef(cur);
    font_begin(BLEND_ADD);
    // crosshair (the weapon's own from crosshairs.spr: a simple one for all)
    if (show_crosshair && w && player_alive() && (w->cx || w->cy || w->id == WEAPON_CROWBAR)) {
        // the crosshair slides onto the monster the auto-aim locked on to (where the shots go)
        extern int npc_autoaim_target;
        static float xh_x = 160, xh_y = 120;
        float tx = 160, ty = 120;
        int t = npc_autoaim_target;
        if (t >= 0 && t < num_npcs && npc_alive(t) && w->id != WEAPON_CROWBAR) {
            npc_t *n = &npcs[t];
            float c[3] = {n->origin[0], n->origin[1], n->origin[2] + (n->mins[2] + n->maxs[2]) * 0.5f}, sx, sy;
            if (project(c, &sx, &sy)) { tx = fminf(fmaxf(sx, 40), 280); ty = fminf(fmaxf(sy, 30), 210); }
        }
        float k = fminf(1, dt * (t >= 0 ? 30 : 18));   // snaps on fast, eases back to the centre
        xh_x += (tx - xh_x) * k; xh_y += (ty - xh_y) * k;
        font_color(RGBA32(255, 160, 0, 255));
        image_draw(&xhair, (int)(xh_x + 0.5f) - 12, (int)(xh_y + 0.5f) - 12, w->cx, w->cy, 24, 24);
    }
    // ammo
    if (w && w->ammo1 >= 0) {
        int clip = w->maxclip > 0 ? player.clip[w->id] : -1, res = player.ammo[w->ammo1];
        if (clip != last_clip || res != last_ammo) { ammo_fade = 255; last_clip = clip; last_ammo = res; }
        if (ammo_fade > 100) ammo_fade -= dt * 20 * 10;
        if (ammo_fade < 100) ammo_fade = 100;
        font_color(hud_color(ammo_fade));
        int y = 240 - 16 - 8 - 8 - HUD_SAFE_Y;
        int x;
        if (clip >= 0) {
            x = 320 - HUD_SAFE_X - 8 * 12 - 24 - 8;
            x = hud_number(x, y, clip, 3, true);
            rdpq_set_mode_fill(hud_color(ammo_fade));
            rdpq_fill_rectangle(x + 6, y, x + 7, y + 16);
            font_begin(BLEND_ADD);
            font_color(hud_color(ammo_fade));
            x += 13;
            x = hud_number(x, y, res, 3, true);
        } else {
            x = 320 - HUD_SAFE_X - 4 * 12 - 24 - 8;
            x = hud_number(x, y, res, 3, true);
        }
        image_draw(&hud2, 320 - HUD_SAFE_X - 24 - 8, y - 2, w->ax, w->ay, 18, 18);
        if (w->ammo2 >= 0) {
            y -= 16 + 4;
            x = 320 - HUD_SAFE_X - 4 * 12 - 24 - 8;
            hud_number(x, y, player.ammo[w->ammo2], 3, true);
            image_draw(&hud2, 320 - HUD_SAFE_X - 24 - 8, y - 2, 36, 16, 18, 18);
        }
    }
    // weapon selection: the slot's weapons under the bucket numbers (CHudAmmo::DrawWList)
    if (select_show > 0) {
        int show = pending ? pending : cur;
        const wdef_t *sw = wdef(show);
        int x = 10 + HUD_SAFE_X, y = 10 + HUD_SAFE_Y;
        for (int s = 0; s < 5; s++) {
            font_color(RGBA32(255, 160, 0, 255));
            image_draw(&hud2, x, y, 108, 16 + s * 12, 12, 12);
            if (sw && sw->slot == s) {
                int yy = y + 14;
                for (int p = 0; p < 5; p++)
                    for (int i = 1; i < NUM_W; i++) {
                        const wdef_t *ww = &W[i];
                        if (!ww->id || ww->slot != s || ww->pos != p || !(player.weapons & (1u << ww->id))) continue;
                        bool sel = ww->id == show;
                        font_color(has_ammo(ww->id) ? RGBA32(sel ? 255 : 160, sel ? 160 : 100, 0, 255) : RGBA32(160, 0, 0, 255));
                        image_draw(&hud1, x, yy, ww->wx, ww->wy + (sel ? 20 : 0), 80, 20);
                        if (sel) image_draw(&hud1, x, yy, 160, 160, 80, 20);
                        yy += 22;
                    }
                x += 84;
            } else {
                x += 14;
            }
        }
    }
    rdpq_set_mode_standard();
}

void weapons_test_select(int id) { if (player.weapons & (1u << id)) deploy(id); }
