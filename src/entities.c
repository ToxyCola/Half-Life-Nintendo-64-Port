#include <ctype.h>
// HL64 - a small subset of the Half-Life game DLL (dlls/) entity logic:
// doors, buttons, plats, triggers, relays, multi_managers, level changes.
// Behaviour follows the SDK sources (doors.cpp, buttons.cpp, triggers.cpp, plats.cpp)
// but is reimplemented in plain C without the CBaseEntity framework.
#include <libdragon.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "bsp.h"
#include "game.h"
#include "npc.h"
#include "hud.h"

#define SF_PATH_DISABLED       0x0001
#define SF_PATH_FIREONCE       0x0002
#define SF_PATH_ALTREVERSE     0x0004
#define SF_PATH_DISABLE_TRAIN  0x0008
#define SF_PATH_ALTERNATE      0x8000
#define SF_PATH_FIRED          0x10000   // ours: FIREONCE message already sent
#define SF_TRACKTRAIN_NOPITCH     0x0001
#define SF_TRACKTRAIN_NOCONTROL   0x0002
#define SF_TRACKTRAIN_FORWARDONLY 0x0004
#define SF_TRACK_ACTIVATETRAIN 0x01
#define SF_TRACK_RELINK        0x02
#define SF_TRACK_STARTBOTTOM   0x08
#define SF_TRACK_DONT_MOVE     0x10
enum { TRAIN_SAFE, TRAIN_BLOCKING, TRAIN_FOLLOWING };
#define PATH_NEAREST (-3)   // tracktrain: find the nearest path_track (after a level transition)
#define PATH_CARRIED (-4)   // tracktrain: riding a func_trackchange

static int driving = -1;       // the train the player controls (PFLAG_ONTRAIN)
static char driving_global[48];

entity_t entities[MAX_ENTITIES];
int num_entities;
static float g_time;
float g_wish_move[2];
static bool pushable_settle_pending;
float game_time(void) { return g_time; }

// player access (pmove.c)
void pm_get_origin(float *o);
void pm_set_origin(const float *o);
void pm_get_angles(float *a);
int  pm_get_groundent(void);
void pm_get_hull(float *mins, float *maxs);
void pm_add_basevelocity(const float *v);
void pm_set_velocity_z(float vz);
void pm_get_velocity(float *v);

// ------------------------------------------------------------------ key/values
const char *ent_value(const entity_t *e, const char *key)
{
    static char val[128];
    const char *p = e->kv;
    if (!p) return NULL;
    while (*p && *p != '}') {
        while (*p && *p != '"' && *p != '}') p++;
        if (*p != '"') break;
        const char *k = ++p;
        while (*p && *p != '"') p++;
        int klen = p - k;
        if (*p) p++;
        while (*p && *p != '"') p++;
        if (!*p) break;
        const char *v = ++p;
        while (*p && *p != '"') p++;
        int vlen = p - v;
        if (*p) p++;
        if ((int)strlen(key) == klen && !strncasecmp(k, key, klen)) {
            if (vlen > 127) vlen = 127;
            memcpy(val, v, vlen);
            val[vlen] = 0;
            return val;
        }
    }
    return NULL;
}

static float ent_float(const entity_t *e, const char *key, float def)
{
    const char *v = ent_value(e, key);
    return v ? atof(v) : def;
}

static void ent_vec(const entity_t *e, const char *key, float *out)
{
    const char *v = ent_value(e, key);
    out[0] = out[1] = out[2] = 0;
    if (v) sscanf(v, "%f %f %f", &out[0], &out[1], &out[2]);
}

static void str_copy(char *dst, const char *src, int n)
{
    if (!src) { dst[0] = 0; return; }
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
}

int ents_find_class(const char *classname, int start)
{
    for (int i = start; i < num_entities; i++)
        if (!strcasecmp(entities[i].classname, classname)) return i;
    return -1;
}

int ents_find_targetname(const char *name, int start)
{
    for (int i = start; i < num_entities; i++)
        if (entities[i].targetname[0] && !strcasecmp(entities[i].targetname, name)) return i;
    return -1;
}

// ------------------------------------------------------------------ math helpers
static void angle_vectors(const float *angles, float *f, float *r, float *u)
{
    float sy = sinf(angles[1] * M_PI / 180), cy = cosf(angles[1] * M_PI / 180);
    float sp = sinf(angles[0] * M_PI / 180), cp = cosf(angles[0] * M_PI / 180);
    float sr = sinf(angles[2] * M_PI / 180), cr = cosf(angles[2] * M_PI / 180);
    if (f) { f[0] = cp * cy; f[1] = cp * sy; f[2] = -sp; }
    if (r) { r[0] = -sr * sp * cy + cr * sy; r[1] = -sr * sp * sy - cr * cy; r[2] = -sr * cp; }
    if (u) { u[0] = cr * sp * cy + sr * sy; u[1] = cr * sp * sy - sr * cy; u[2] = cr * cp; }
}

// util.cpp SetMovedir()
static void set_movedir(entity_t *e)
{
    if (e->angles[0] == 0 && e->angles[1] == -1 && e->angles[2] == 0) {
        e->movedir[0] = 0; e->movedir[1] = 0; e->movedir[2] = 1;
    } else if (e->angles[0] == 0 && e->angles[1] == -2 && e->angles[2] == 0) {
        e->movedir[0] = 0; e->movedir[1] = 0; e->movedir[2] = -1;
    } else {
        angle_vectors(e->angles, e->movedir, NULL, NULL);
    }
    e->angles[0] = e->angles[1] = e->angles[2] = 0;
}

static void model_size(entity_t *e, float *size)
{
    bmodel_t *bm = &world.bmodels[e->model];
    for (int i = 0; i < 3; i++) size[i] = bm->maxs[i] - bm->mins[i];
}

// ------------------------------------------------------------------ sounds (doors.cpp / buttons.cpp tables)
static const char *door_move_sound(int n)
{
    static const char *t[] = {NULL, "doors/doormove1.wav", "doors/doormove2.wav", "doors/doormove3.wav",
                              "doors/doormove4.wav", "doors/doormove5.wav", "doors/doormove6.wav",
                              "doors/doormove7.wav", "doors/doormove8.wav", "doors/doormove9.wav",
                              "doors/doormove10.wav"};
    return (n > 0 && n <= 10) ? t[n] : NULL;
}
static const char *door_stop_sound(int n)
{
    static const char *t[] = {NULL, "doors/doorstop1.wav", "doors/doorstop2.wav", "doors/doorstop3.wav",
                              "doors/doorstop4.wav", "doors/doorstop5.wav", "doors/doorstop6.wav",
                              "doors/doorstop7.wav", "doors/doorstop8.wav"};
    return (n > 0 && n <= 8) ? t[n] : NULL;
}
static const char *button_sound(int n)
{
    static char buf[32];
    if (n <= 0 || n > 14) return NULL;
    snprintf(buf, sizeof(buf), "buttons/button%d.wav", n == 14 ? 9 : n);
    if (n == 10) return "buttons/button10.wav";
    if (n == 11) return "buttons/button11.wav";
    return buf;
}

// ------------------------------------------------------------------ event queue (delayed target firing)
#define MAX_EVENTS 256
typedef struct { char name[48]; float time; int activator; int caller; } event_t;
static event_t events[MAX_EVENTS];
static int num_events;
static int fire_depth;
static int cur_activator = -1;   // entity that caused the current FireTargets (for func_trackautochange)
static int cur_caller = -1;      // entity whose target is being fired (pCaller: multisource sources)

// ---- global states (CGlobalState: env_global, multisource/trigger_auto "globalstate"); they live
// across maps and in saved games
#define MAX_GLOBALS 24
typedef struct { char name[40]; int8_t state; } gvar_t;   // 0 off, 1 on, 2 dead
static gvar_t gvars[MAX_GLOBALS];
static int num_gvars;
static int gstate_find(const char *name)
{
    for (int i = 0; i < num_gvars; i++) if (!strcasecmp(gvars[i].name, name)) return i;
    return -1;
}
int ents_global_state(const char *name)   // -1 unknown
{
    int i = name && name[0] ? gstate_find(name) : -1;
    return i < 0 ? -1 : gvars[i].state;
}
static void gstate_set(const char *name, int state)
{
    if (!name || !name[0]) return;
    int i = gstate_find(name);
    if (i < 0) {
        if (num_gvars >= MAX_GLOBALS) return;
        i = num_gvars++;
        strncpy(gvars[i].name, name, sizeof(gvars[i].name) - 1);
    }
    gvars[i].state = state;
    debugf("global %s = %d\n", name, state);
}
void ents_globals_reset(void) { num_gvars = 0; memset(gvars, 0, sizeof(gvars)); }
int ents_globals_save(uint8_t *b, int max)
{
    int n = num_gvars * (int)sizeof(gvar_t) + 1;
    if (n > max) return 0;
    b[0] = num_gvars;
    memcpy(b + 1, gvars, num_gvars * sizeof(gvar_t));
    return n;
}
void ents_globals_load(const uint8_t *b, int len)
{
    ents_globals_reset();
    if (len < 1) return;
    int n = b[0];
    if (n > MAX_GLOBALS || 1 + n * (int)sizeof(gvar_t) > len) return;
    num_gvars = n;
    memcpy(gvars, b + 1, n * sizeof(gvar_t));
}
static bool global_ok(entity_t *e)   // an entity with "globalstate" works only while that state is on
{
    const char *g = ent_value(e, "globalstate");
    if (!g || !g[0]) return true;
    char nm[40];
    str_copy(nm, g, sizeof(nm));
    return ents_global_state(nm) == 1;
}

// ---- multisource (CMultiSource): triggered when every entity that targets it has switched it on
#define MAX_MSOURCES 64
typedef struct { int16_t ent, n; int16_t src[24]; uint32_t on; } msrc_t;
static msrc_t msrc[MAX_MSOURCES];
static int num_msrc;
static msrc_t *msrc_for(int ent) { for (int i = 0; i < num_msrc; i++) if (msrc[i].ent == ent) return &msrc[i]; return NULL; }
static bool msource_triggered(entity_t *ms)
{
    msrc_t *m = msrc_for((int)(ms - entities));
    if (m) {
        uint32_t all = m->n >= 32 ? 0xffffffffu : ((1u << m->n) - 1);
        if ((m->on & all) != all) return false;
    } else if (ms->msource_count < ms->code) return false;
    return global_ok(ms);
}

static bool in_event_pass;

static void queue_event(const char *name, float time)
{
    if (!name || !name[0] || num_events >= MAX_EVENTS) return;
    // events raised while the queue is being run wait for the next frame, so a cycle of
    // zero-delay targets can't spin forever within one frame
    if (in_event_pass && time <= g_time) time = g_time + 0.0005f;
    str_copy(events[num_events].name, name, 48);
    events[num_events].time = time;
    events[num_events].activator = cur_activator;
    events[num_events].caller = cur_caller;
    num_events++;
}

static void queue_fire(const char *name, float delay)
{
    if (!name || !name[0]) return;
    // fire immediately like FireTargets, but defer deep chains to the next frame
    if (delay <= 0 && fire_depth < 8) { ents_fire_targets(name); return; }
    queue_event(name, g_time + (delay > 0 ? delay : 0));
}

static void ent_use(entity_t *e);
static void train_next(entity_t *e);
static void trackchange_use(entity_t *e);
static void tt_use(entity_t *e, int idx);
static void path_track_use(entity_t *e);
static void path_link_all(void);
static void trackchange_find(entity_t *e);
static bool master_ok(entity_t *e);
static void special_use(entity_t *e);
void game_spawn_gibs(const float *o, int count, bool alien, float speed);
void game_effect_particles(const float *o, int count, float speed, int r, int g, int b, float life, float grav);
static void ambient_use(entity_t *e);
static void play_cd_track(int track);
static void spark_sound(entity_t *e);
typedef struct { int16_t ia, ib; uint8_t c[4]; bool laser, init, plain; float radius, strike; } beam_cache_t;
static beam_cache_t beam_cache[MAX_ENTITIES];   // env_beam endpoints, resolved on first use
void pm_set_ontrain(bool on);
void pm_set_velocity(const float *v);

// CChangeLevel "changetarget": a CFireAndDie is carried into the next map and fires it there
// after "changedelay" (Office Complex: starts the elevator you arrive in)
static char carry_target[48];
static float carry_delay;
static void request_changelevel(entity_t *e)
{
    const char *ct = ent_value(e, "changetarget");
    str_copy(carry_target, ct ? ct : "", sizeof(carry_target));
    carry_delay = ent_float(e, "changedelay", 0);
    char map[32], lm[48];
    str_copy(map, ent_value(e, "map"), sizeof(map));
    str_copy(lm, ent_value(e, "landmark"), sizeof(lm));
    game_request_changelevel(map, lm);
}

void ents_fire_targets(const char *name)
{
    if (!name || !name[0]) return;
    debugf("[%.1f] fire %s\n", g_time, name);
    fire_depth++;
    for (int i = ents_find_targetname(name, 1); i >= 0; i = ents_find_targetname(name, i + 1))
        ent_use(&entities[i]);
    fire_depth--;
}

#define fire_entity_target ents_fire_entity_target
void ents_fire_entity_target(entity_t *e)
{
    const char *kt = ent_value(e, "killtarget");
    if (kt) {
        char name[48];
        str_copy(name, kt, sizeof(name));
        for (int i = ents_find_targetname(name, 1); i >= 0; i = ents_find_targetname(name, i + 1))
            entities[i].removed = true, entities[i].visible = false, entities[i].solid = SOLID_NOT;
    }
    int oldc = cur_caller;
    cur_caller = (int)(e - entities);
    queue_fire(e->target, e->delay);
    cur_caller = oldc;
}

// ------------------------------------------------------------------ movers
static void move_linear(entity_t *e, const float *dest)
{
    memcpy(e->dest, dest, 12);
    e->angular = false;
    e->active = true;
}

static void move_angular(entity_t *e, const float *dest)
{
    memcpy(e->dest, dest, 12);
    e->angular = true;
    e->active = true;
}

// doors.cpp / plats.cpp: the moving sound loops on CHAN_STATIC, the stop sound plays on arrival
static const char *plat_move_sound(int n)
{
    static const char *t[] = {NULL, "plats/bigmove1.wav", "plats/bigmove2.wav", "plats/elevmove1.wav",
                              "plats/elevmove2.wav", "plats/elevmove3.wav", "plats/freightmove1.wav",
                              "plats/freightmove2.wav", "plats/heavymove1.wav", "plats/rackmove1.wav",
                              "plats/railmove1.wav", "plats/squeekmove1.wav", "plats/talkmove1.wav", "plats/talkmove2.wav"};
    return (n > 0 && n <= 13) ? t[n] : NULL;
}
static const char *plat_stop_sound(int n)
{
    static const char *t[] = {NULL, "plats/bigstop1.wav", "plats/bigstop2.wav", "plats/freightstop1.wav",
                              "plats/heavystop2.wav", "plats/rackstop1.wav", "plats/railstop1.wav",
                              "plats/squeekstop1.wav", "plats/talkstop1.wav"};
    return (n > 0 && n <= 8) ? t[n] : NULL;
}
static bool is_door(entity_t *e) { return e->type == ENT_DOOR || e->type == ENT_DOOR_ROTATING; }
static const char *mover_move_sound(entity_t *e)
{
    return is_door(e) ? door_move_sound(e->movesnd) : plat_move_sound(e->movesnd);
}
static void mover_sound_start(entity_t *e)
{
    const char *m = mover_move_sound(e);
    if (!m) return;
    if (e->type == ENT_TRAIN && snd_playing(e - entities, CHAN_STATIC)) return;   // already rolling
    snd_stop(e - entities, CHAN_STATIC, m);
    snd_emit(e - entities, CHAN_STATIC, m, e->type == ENT_TRAIN ? 0.85f : 1.0f, ATTN_NORM, PITCH_NORM, NULL);
}
static void mover_sound_stop(entity_t *e)
{
    const char *m = mover_move_sound(e);
    if (m) snd_stop(e - entities, CHAN_STATIC, m);
    const char *st = is_door(e) ? door_stop_sound(e->stopsnd) : plat_stop_sound(e->stopsnd);
    if (st) snd_emit(e - entities, is_door(e) ? CHAN_STATIC : CHAN_WEAPON, st, 1.0f, ATTN_NORM, PITCH_NORM, NULL);
}

static void door_go_up(entity_t *e, bool by_player)
{
    if (e->state == MS_GOING_UP) return;
    if (e->state == MS_AT_TOP) {
        if (e->wait >= 0) e->nextthink = g_time + e->wait; // reset the auto-return timer
        return;
    }
    e->state = MS_GOING_UP;
    mover_sound_start(e);
    if (e->type == ENT_DOOR_ROTATING) {
        float sign = 1.0f;
        if (by_player && !(e->spawnflags & 16) && e->movedir[1] != 0) {
            // doors.cpp DoorGoUp: open away from the activator
            float po[3], pa[3], f[3];
            pm_get_origin(po);
            pm_get_angles(pa);
            pa[0] = pa[2] = 0;
            angle_vectors(pa, f, NULL, NULL);
            float vec[2] = {po[0] - e->origin[0], po[1] - e->origin[1]};
            float vnext[2] = {po[0] + f[0] * 10 - e->origin[0], po[1] + f[1] * 10 - e->origin[1]};
            if (vec[0] * vnext[1] - vec[1] * vnext[0] < 0) sign = -1.0f;
        }
        e->act_sign = sign;
        float d[3];
        for (int i = 0; i < 3; i++) d[i] = e->ang1[i] + (e->ang2[i] - e->ang1[i]) * sign;
        move_angular(e, d);
    } else {
        move_linear(e, e->pos2);
    }
}

static void door_go_down(entity_t *e)
{
    if (e->state == MS_GOING_DOWN || e->state == MS_AT_BOTTOM) return;
    e->state = MS_GOING_DOWN;
    mover_sound_start(e);
    if (e->type == ENT_DOOR_ROTATING) move_angular(e, e->ang1);
    else move_linear(e, e->pos1);
}

static void mover_reached(entity_t *e)
{
    if (is_door(e) || e->type == ENT_PLAT) mover_sound_stop(e);
    if (e->state == MS_GOING_UP) {
        e->state = MS_AT_TOP;
        if (e->type == ENT_DOOR || e->type == ENT_DOOR_ROTATING) {
            if (e->wait >= 0 && !(e->spawnflags & 32)) e->nextthink = g_time + e->wait;
            else e->nextthink = 0;
            if (e->type == ENT_DOOR || e->type == ENT_DOOR_ROTATING) fire_entity_target(e);
            // CBaseDoor::DoorHitTop: "netname" (fire on close) for start-open doors
            const char *nn = ent_value(e, "netname");
            if (nn && (e->spawnflags & 1)) ents_fire_targets(nn);
        } else if (e->type == ENT_PLAT) {
            e->nextthink = g_time + 3.0f;
        } else if (e->type == ENT_BUTTON || e->type == ENT_ROT_BUTTON) {
            fire_entity_target(e);
            if (e->wait >= 0) e->nextthink = g_time + e->wait;
        }
    } else if (e->state == MS_GOING_DOWN) {
        e->state = MS_AT_BOTTOM;
        e->nextthink = 0;
        // CBaseDoor::DoorHitBottom: "netname" fires when the door is back shut (the Office
        // Complex elevator door: closing it starts the level change to c1a2)
        if (is_door(e)) {
            const char *nn = ent_value(e, "netname");
            if (nn && !(e->spawnflags & 1)) ents_fire_targets(nn);
        }
    }
}


// ------------------------------------------------------------------ masters (multisource)
#define MAX_PUSHABLES 64
static int num_pushables;   // func_pushables in the map (triggers that pushables can set off)
static uint16_t pushable_list[MAX_PUSHABLES];
static int16_t master_cache[MAX_ENTITIES];   // 0 unresolved, -1 no master, else the multisource
static bool master_ok(entity_t *e)
{
    int ei = (int)(e - entities);
    int i = master_cache[ei];
    if (i == 0) {
        const char *m = ent_value(e, "master");
        if (!m || !m[0]) i = -1;
        else {
            char name[48];
            str_copy(name, m, sizeof(name));
            i = ents_find_targetname(name, 1);
            if (i <= 0) i = -1;
        }
        master_cache[ei] = i;
    }
    if (i < 0) return true;   // "Master was null or not a master!" -> treated as triggered
    entity_t *ms = &entities[i];
    if (ms->type != ENT_MULTISOURCE) return true;
    return msource_triggered(ms);
}

static void parse_color(const entity_t *e, const char *key, uint8_t *c, int n)
{
    const char *v = ent_value(e, key);
    int r = 255, g = 255, b = 255, a = 255;
    if (v) sscanf(v, "%d %d %d %d", &r, &g, &b, &a);
    c[0] = r; c[1] = g; c[2] = b;
    if (n > 3) c[3] = a;
}

// doors.cpp / buttons.cpp: "locked_sound" when the master isn't satisfied
static void door_locked_sound(entity_t *e)
{
    int n = (int)ent_float(e, "locked_sound", 0);
    if (n <= 0) return;
    if (g_time < e->sndtime) return;
    e->sndtime = g_time + 1.0f;
    snd_emit(e - entities, CHAN_ITEM, button_sound(n), 1.0f, ATTN_NORM, PITCH_NORM, NULL);
}

// ------------------------------------------------------------------ sound entities (sound.cpp)
static float ambient_attenuation(entity_t *e)
{
    if (e->spawnflags & 1) return ATTN_NONE;     // AMBIENT_SOUND_EVERYWHERE
    if (e->spawnflags & 2) return ATTN_IDLE;     // small radius
    if (e->spawnflags & 4) return ATTN_STATIC;   // medium
    if (e->spawnflags & 8) return ATTN_NORM;     // large
    return ATTN_STATIC;
}

static void ambient_play(entity_t *e)
{
    char msg[64];
    str_copy(msg, ent_value(e, "message"), sizeof(msg));
    if (!msg[0]) return;
    float vol = e->health * 0.1f;
    if (vol > 1) vol = 1;
    int pitch = (int)ent_float(e, "pitch", 100);
    if (pitch <= 0) pitch = 100;
    int id = snd_emit(e - entities, CHAN_STATIC, msg, vol, ambient_attenuation(e), pitch, e->origin);
    e->snd = id + 1;
}

static void ambient_stop(entity_t *e)
{
    char msg[64];
    str_copy(msg, ent_value(e, "message"), sizeof(msg));
    snd_stop(e - entities, CHAN_STATIC, msg[0] ? msg : NULL);
    e->snd = 0;
}

// CAmbientGeneric::ToggleUse
static void ambient_use(entity_t *e)
{
    bool looping = !(e->spawnflags & 32);
    if (e->active) {
        e->active = false;
        ambient_stop(e);
        return;
    }
    if (looping) e->active = true;
    else ambient_stop(e);
    ambient_play(e);
}

static void play_cd_track(int track)
{
    if (track == -1) snd_music_stop();          // "cd pause"
    else if (track >= 2 && track <= 30) snd_music_play(track);
}

void game_effect_sparks(const float *o);
void game_effect_lightning(const float *a, const float *b, int r, int g, int bl);
static void spark_sound(entity_t *e)
{
    game_effect_sparks(e->origin);
    char s[32];
    snprintf(s, sizeof(s), "buttons/spark%d.wav", 1 + rand() % 6);
    snd_emit(e - entities, CHAN_VOICE, s, 0.1f + (rand() % 100) / 160.0f, ATTN_NORM, PITCH_NORM, NULL);
}

// CScriptedSentence::FindEntity / AcceptableSpeaker
static int sentence_speaker(entity_t *e)
{
    char want[48];
    str_copy(want, ent_value(e, "entity"), sizeof(want));
    if (!want[0]) return -1;
    bool override = (e->spawnflags & 4) != 0, followers = (e->spawnflags & 2) != 0;
    for (int i = ents_find_targetname(want, 1); i >= 0; i = ents_find_targetname(want, i + 1)) {
        int n = npc_for_entity(i);
        if (n >= 0 && npc_can_speak(n, override, followers)) return n;
    }
    float radius = ent_float(e, "radius", 512);
    for (int i = 1; i < num_entities; i++) {
        if (entities[i].removed || strcasecmp(entities[i].classname, want)) continue;
        int n = npc_for_entity(i);
        if (n < 0) continue;
        float o[3];
        npc_origin_for_entity(i, o);
        float d2 = 0;
        for (int k = 0; k < 3; k++) d2 += (o[k] - e->origin[k]) * (o[k] - e->origin[k]);
        if (d2 <= radius * radius && npc_can_speak(n, override, followers)) return n;
    }
    return -1;
}

// CScriptedSentence::FindThink
static void sentence_find_think(entity_t *e)
{
    int n = sentence_speaker(e);
    float refire = ent_float(e, "refire", 3);
    if (n < 0) { e->nextthink = g_time + refire + 0.5f; return; }
    char sent[48], lis[48];
    str_copy(sent, ent_value(e, "sentence"), sizeof(sent));
    str_copy(lis, ent_value(e, "listener"), sizeof(lis));
    float dur = ent_float(e, "duration", 3);
    float vol = ent_float(e, "volume", 10) * 0.1f;
    if (vol <= 0) vol = 1;
    static const float attn[4] = {ATTN_NORM, ATTN_IDLE, ATTN_STATIC, ATTN_NONE};
    int a = (int)ent_float(e, "attenuation", 0);
    int lent = -1;
    if (lis[0]) {
        if (!strcasecmp(lis, "player")) lent = 0;
        else lent = ents_find_targetname(lis, 1);
        if (lent < 0) lent = ents_find_class(lis, 1);
    }
    npc_play_scripted_sentence(n, sent, dur, vol, attn[a & 3], !(e->spawnflags & 8), lent);
    fire_entity_target(e);
    if (e->spawnflags & 1) { e->removed = true; return; }   // SF_SENTENCE_ONCE
    e->triggered = false;                                     // m_active = FALSE
    e->nextthink = 0;
    e->ltime = g_time + dur + refire;                         // DelayThink
}

// CSpeaker::SpeakerThink
static void speaker_think(entity_t *e)
{
    if (g_time <= talk_wait_time) { e->nextthink = talk_wait_time + 5 + rand() % 6; return; }
    static const char *presets[] = {NULL, "C1A0_", "C1A1_", "C1A2_", "C1A3_", "C1A4_", "C2A1_", "C2A2_", "C2A3_",
                                    "C2A4_", "C2A5_", "C3A1_", "C3A2_"};
    char msg[48];
    int preset = (int)ent_float(e, "preset", 0);
    if (preset > 0 && preset <= 12) str_copy(msg, presets[preset], sizeof(msg));
    else str_copy(msg, ent_value(e, "message"), sizeof(msg));
    float vol = e->health * 0.1f;
    if (msg[0] == '!') {
        snd_emit(e - entities, CHAN_STATIC, msg, vol, 0.3f, PITCH_NORM, e->origin);
        e->nextthink = 0;
    } else if (msg[0]) {
        int id = snd_sentence(e - entities, msg, vol, 0.3f, PITCH_NORM);
        (void)id;
        e->nextthink = g_time + 15 + (rand() % 1200) / 10.0f;   // 0.25 .. 2.25 minutes
        talk_wait_time = g_time + 5;
    }
}

// point entities that talk to the HUD / camera / renderer
static void special_use(entity_t *e)
{
    switch (e->type) {
    case ENT_MULTISOURCE:
    {   // CMultiSource::Use: the calling source toggles its switch; all on (and the global state) fires
        msrc_t *m = msrc_for((int)(e - entities));
        if (m) {
            int k = -1;
            for (int j = 0; j < m->n && j < 24; j++) if (m->src[j] == cur_caller) { k = j; break; }
            if (k < 0) for (int j = 0; j < m->n && j < 24; j++) if (!(m->on & (1u << j))) { k = j; break; }   // caller unknown
            if (k >= 0) m->on ^= 1u << k;
        } else e->msource_count++;
        if (msource_triggered(e)) fire_entity_target(e);
    }
        break;
    case ENT_ENV_FADE: {
        // effects.cpp CFade::Use
        uint8_t c[4];
        parse_color(e, "rendercolor", c, 3);
        c[3] = (uint8_t)ent_float(e, "renderamt", 255);
        int flags = 0;
        if (!(e->spawnflags & 1)) flags |= FFADE_OUT;     // SF_FADE_IN
        if (e->spawnflags & 2) flags |= FFADE_MODULATE;
        hud_fade(c, ent_float(e, "duration", 2), ent_float(e, "holdtime", 0), flags);
        fire_entity_target(e);
        break;
    }
    case ENT_ENV_MESSAGE: {
        // effects.cpp CMessage::Use
        char m[64];
        str_copy(m, ent_value(e, "message"), sizeof(m));
        hud_show_message(m);
        const char *ms = ent_value(e, "messagesound");
        if (ms && ms[0]) {
            char snd[64];
            str_copy(snd, ms, sizeof(snd));
            float vol = ent_float(e, "messagevolume", 10) * 0.1f;
            if (vol <= 0) vol = 1;
            static const float attn[4] = {ATTN_NORM, ATTN_IDLE, ATTN_STATIC, ATTN_NONE};
            int a = (int)ent_float(e, "messageattenuation", 0);
            snd_emit(e - entities, CHAN_BODY, snd, vol, attn[a & 3], PITCH_NORM, NULL);
        }
        if (e->spawnflags & 1) e->removed = true;          // SF_MESSAGE_ONCE
        fire_entity_target(e);
        break;
    }
    case ENT_GAME_TEXT: {
        // maprules.cpp CGameText
        static char txt[128];
        str_copy(txt, ent_value(e, "message"), sizeof(txt));
        textmsg_t m = {.x = ent_float(e, "x", -1), .y = ent_float(e, "y", -1), .effect = (int)ent_float(e, "effect", 0),
                       .fadein = ent_float(e, "fadein", 1.5f), .fadeout = ent_float(e, "fadeout", 0.5f),
                       .holdtime = ent_float(e, "holdtime", 1.2f), .fxtime = ent_float(e, "fxtime", 0.25f),
                       .text = txt};
        uint8_t c[4];
        parse_color(e, "color", c, 4);
        m.r1 = c[0]; m.g1 = c[1]; m.b1 = c[2];
        parse_color(e, "color2", c, 4);
        m.r2 = c[0]; m.g2 = c[1]; m.b2 = c[2];
        hud_show_text(&m);
        fire_entity_target(e);
        break;
    }
    case ENT_CAMERA: {
        // triggers.cpp CTriggerCamera::Use (toggles)
        int idx = (int)(e - entities);
        if (e->active) { e->active = false; e->nextthink = 0; game_set_camera(-1, 0, false); break; }
        e->active = true;
        float wait = ent_float(e, "wait", 10);
        e->nextthink = g_time + wait;
        game_set_camera(idx, wait, (e->spawnflags & 4) != 0);   // SF_CAMERA_PLAYER_TAKECONTROL
        break;
    }
    case ENT_SENTENCE:
        // scripted.cpp CScriptedSentence::Use: look for the speaker now (only while active)
        if (e->triggered) e->nextthink = g_time + 0.0001f;
        break;
    case ENT_AMBIENT:
        ambient_use(e);
        break;
    case ENT_SPEAKER:
        // sound.cpp CSpeaker::ToggleUse
        e->nextthink = e->nextthink > 0 ? 0 : g_time + 0.1f;
        break;
    case ENT_CDAUDIO:
    case ENT_TARGET_CDAUDIO:
        // CTriggerCDAudio::PlayTrack / CTargetCDAudio::Play: once, then removed
        play_cd_track((int)e->health);
        e->removed = true;
        break;
    case ENT_ENV_SHAKE: {
        // effects.cpp CShake::Use -> UTIL_ScreenShake
        float amp = ent_float(e, "amplitude", 4), freq = ent_float(e, "frequency", 40), dur = ent_float(e, "duration", 1);
        float rad = ent_float(e, "radius", 500);
        float po[3];
        pm_get_origin(po);
        float d = sqrtf((po[0] - e->origin[0]) * (po[0] - e->origin[0]) + (po[1] - e->origin[1]) * (po[1] - e->origin[1]) +
                        (po[2] - e->origin[2]) * (po[2] - e->origin[2]));
        if (e->spawnflags & 1) game_shake(amp, freq, dur);          // SF_SHAKE_EVERYONE
        else if (d < rad) game_shake(amp * (1 - d / rad), freq, dur);
        break;
    }
    case ENT_ENV_SHOOTER: {
        // effects.cpp CGibShooter/CEnvShooter: a burst of gibs or material debris
        int n = (int)ent_float(e, "m_iGibs", 3);
        float v = ent_float(e, "m_flVelocity", 200);
        const char *mdl = ent_value(e, "shootmodel");
        int mat = (int)ent_float(e, "shootsounds", -1);
        if (n < 1) n = 1;
        if (n > 12) n = 12;
        if (mdl && strstr(mdl, "gibs") && (strstr(mdl, "hgib") || strstr(mdl, "agib") || mat == 3))
            game_spawn_gibs(e->origin, n > 4 ? 4 : n, strstr(mdl, "agib") != NULL, v * 0.6f);
        else {
            static const uint8_t col[5][3] = {{180, 210, 230}, {140, 100, 60}, {150, 150, 150}, {150, 20, 20}, {120, 110, 100}};
            int m = mat >= 0 && mat < 5 ? mat : 4;
            if (mdl && strstr(mdl, "glass")) m = 0;
            else if (mdl && (strstr(mdl, "wood") || strstr(mdl, "crate"))) m = 1;
            else if (mdl && (strstr(mdl, "metal") || strstr(mdl, "computer"))) m = 2;
            game_effect_particles(e->origin, n * 4, v * 0.8f, col[m][0], col[m][1], col[m][2], 1.2f, 600);
            if (mdl && strstr(mdl, "spr")) game_effect_sparks(e->origin);
        }
        static const char *ms[5] = {"debris/glass1.wav", "debris/wood1.wav", "debris/metal1.wav", "debris/flesh1.wav", "debris/concrete1.wav"};
        if (mat >= 0 && mat < 5) snd_emit(-1, CHAN_AUTO, ms[mat], 0.8f, 0.8f, PITCH_NORM, e->origin);
        if (!(e->spawnflags & 1)) e->removed = true;     // SF_GIBSHOOTER_REPEATABLE
        break;
    }
    case ENT_FUNNEL: {
        // effects.cpp CEnvFunnel: a sucking (or with SF 1 spraying) cloud of green sparks
        for (int k = 0; k < 4; k++) {
            float o[3] = {e->origin[0] + (rand() % 200 - 100), e->origin[1] + (rand() % 200 - 100), e->origin[2] + (rand() % 100)};
            game_effect_particles(o, 24, 260, 120, 255, 120, 1.6f, (e->spawnflags & 1) ? 0 : -200);
        }
        break;
    }
    case ENT_ENV_BLOOD: {
        extern void game_effect_blood_color(const float *o, bool yellow);
        game_effect_blood_color(e->origin, (int)ent_float(e, "color", 0) == 1);
        break;
    }
    case ENT_BEVERAGE: {
        // CEnvBeverage: each use drops a can worth 1 health; "health" cans in the machine
        if (e->health <= 0) e->health = ent_float(e, "health", 10) > 0 ? ent_float(e, "health", 10) : 10;
        if (e->wait > 0 || g_time < e->ltime) break;
        e->ltime = g_time + 1.0f;
        if (--e->health <= 0) e->wait = 1;
        snd_emit(-1, CHAN_AUTO, "items/smallmedkit1.wav", 1.0f, 0.8f, PITCH_NORM, e->origin);
        player_take_health(1, 0);
        break;
    }
    case ENT_WEAPONSTRIP:
        player.weapons &= (1u << 31);    // keep the suit bit
        player.cur_weapon = 0;
        break;
    case ENT_ENV_SPARK:
        // effects.cpp CEnvSpark: toggled, or one spark per use
        if (e->spawnflags & 32) { e->active = !e->active; e->nextthink = e->active ? g_time + 0.1f : 0; }
        else spark_sound(e);
        break;
    case ENT_ENV_EXPLOSION: {
        // explode.cpp CEnvExplosion::Use
        static const char *ex[] = {"weapons/explode3.wav", "weapons/explode4.wav", "weapons/explode5.wav"};
        snd_emit(-1, CHAN_AUTO, ex[rand() % 3], 1.0f, 0.3f, PITCH_NORM, e->origin);
        float mag = ent_float(e, "iMagnitude", 0);
        if (!(e->spawnflags & 1)) game_radius_damage(e->origin, mag, mag * 2.5f, -1, DMG_BLAST);
        game_effect_explosion(e->origin, mag);
        if (!(e->spawnflags & 2)) e->removed = true;               // SF_ENVEXPLOSION_REPEATABLE
        break;
    }
    case ENT_ENV_RENDER: {
        // effects.cpp CRenderFxManager::Use
        int amt = (int)ent_float(e, "renderamt", 255), mode = (int)ent_float(e, "rendermode", 0);
        for (int i = ents_find_targetname(e->target, 1); i >= 0; i = ents_find_targetname(e->target, i + 1)) {
            entity_t *t = &entities[i];
            if (!(e->spawnflags & 2)) t->renderamt = amt;      // SF_RENDER_MASKAMT
            if (!(e->spawnflags & 4)) t->rendermode = mode;    // SF_RENDER_MASKMODE
            if (t->model > 0 && t->type != ENT_INVISIBLE_BRUSH && t->type < ENT_TRIGGER_ONCE)
                t->visible = !(t->rendermode != 0 && t->renderamt == 0 && t->rendermode != 4);
        }
        break;
    }
    case ENT_ENDSECTION:
        game_end_section();
        break;
    default:
        break;
    }
}

// ------------------------------------------------------------------ func_breakable (func_break.cpp)
static const char *break_spawn_objects[] = {
    NULL, "item_battery", "item_healthkit", "weapon_9mmhandgun", "ammo_9mmclip", "weapon_9mmAR", "ammo_9mmAR",
    "ammo_ARgrenades", "weapon_shotgun", "ammo_buckshot", "weapon_crossbow", "ammo_crossbow", "weapon_357",
    "ammo_357", "weapon_rpg", "ammo_rpgclip", "ammo_gaussclip", "weapon_handgrenade", "weapon_tripmine",
    "weapon_satchel", "weapon_snark", "weapon_hornetgun",
};

static void breakable_center(entity_t *e, float *c)
{
    bmodel_t *bm = &world.bmodels[e->model > 0 ? e->model : 0];
    for (int k = 0; k < 3; k++) c[k] = e->origin[k] + (bm->mins[k] + bm->maxs[k]) * 0.5f;
}

// CBreakable::Die
static void breakable_die(entity_t *e)
{
    if (e->removed) return;
    int mat = (int)ent_float(e, "material", 0);
    char s[40];
    int pitch = 95 + rand() % 30;
    float vol = 0.5f + (rand() % 50) / 100.0f;
    switch (mat) {
    case 0: case 7: snprintf(s, sizeof(s), "debris/bustglass%d.wav", 1 + rand() % 2); break;
    case 1: snprintf(s, sizeof(s), "debris/bustcrate%d.wav", 1 + rand() % 2); break;
    case 2: case 6: snprintf(s, sizeof(s), "debris/bustmetal%d.wav", 1 + rand() % 2); break;
    case 3: snprintf(s, sizeof(s), "debris/bustflesh%d.wav", 1 + rand() % 2); break;
    case 4: case 8: snprintf(s, sizeof(s), "debris/bustconcrete%d.wav", 1 + rand() % 2); break;
    case 5: snprintf(s, sizeof(s), "debris/bustceiling.wav"); break;
    default: s[0] = 0;
    }
    float c[3];
    breakable_center(e, c);
    if (s[0]) snd_emit(-1, CHAN_VOICE, s, vol, ATTN_NORM, pitch, c);
    e->removed = true; e->visible = false; e->solid = SOLID_NOT;
    game_effect_debris(c, mat);
    // the item it holds (m_iszSpawnObject)
    int so = (int)ent_float(e, "spawnobject", 0);
    if (so > 0 && so < (int)(sizeof(break_spawn_objects) / sizeof(break_spawn_objects[0]))) {
        bmodel_t *bm = &world.bmodels[e->model > 0 ? e->model : 0];
        float o[3] = {c[0], c[1], e->origin[2] + bm->mins[2]};
        npc_spawn_item(break_spawn_objects[so], o, 0);
    }
    float mag = ent_float(e, "explodemagnitude", 0);
    if (mag > 0) {
        snd_emit(-1, CHAN_AUTO, "weapons/explode3.wav", 1.0f, 0.3f, PITCH_NORM, c);
        game_radius_damage(c, mag, mag * 2.5f, -1, DMG_BLAST);
        game_effect_explosion(c, mag);
    }
    fire_entity_target(e);
}

bool ents_brush_is_breakable(int ent)
{
    entity_t *e = &entities[ent];
    return !e->removed && (e->type == ENT_BREAKABLE || e->type == ENT_PUSHABLE) && e->maxhealth > 0 &&
           !(e->type == ENT_BREAKABLE && (e->spawnflags & 1));
}

// CBreakable::TakeDamage (bullets, explosions, the crowbar)
void ents_damage_brush(int ent, float dmg, int dmgtype, const float *dir)
{
    if (ent <= 0 || ent >= num_entities) return;
    entity_t *e = &entities[ent];
    if (!ents_brush_is_breakable(ent)) {
        // a hit on unbreakable glass/metal still makes its impact sound
        return;
    }
    if (e->type == ENT_BREAKABLE && (e->spawnflags & 256) && (dmgtype & DMG_CLUB)) dmg = e->health;  // SF_BREAK_CROWBAR
    if (dmgtype & DMG_BLAST) dmg *= 1.0f;
    int mat = (int)ent_float(e, "material", 0);
    char s[40] = "";
    switch (mat) {
    case 0: case 7: snprintf(s, sizeof(s), "debris/glass%d.wav", 1 + rand() % 3); break;
    case 1: snprintf(s, sizeof(s), "debris/wood%d.wav", 1 + rand() % 3); break;
    case 2: case 6: snprintf(s, sizeof(s), "debris/metal%d.wav", 1 + rand() % 3); break;
    case 3: snprintf(s, sizeof(s), "debris/flesh%d.wav", 1 + rand() % 7); break;
    case 4: case 8: snprintf(s, sizeof(s), "debris/concrete%d.wav", 1 + rand() % 3); break;
    }
    float c[3];
    breakable_center(e, c);
    e->health -= dmg;
    if (e->health <= 0) { breakable_die(e); return; }
    if (s[0] && (rand() & 1)) snd_emit(-1, CHAN_VOICE, s, 0.9f, ATTN_NORM, 95 + rand() % 30, c);
}

void game_effect_blood(const float *o);
void game_effect_particles(const float *o, int count, float speed, int r, int g, int b, float life, float grav);
// ---- func_tank (func_tank.cpp): a mounted gun that turns within its yaw/pitch ranges toward the
// player (SF_TANK_ACTIVE + SF_TANK_PLAYER) and fires when on target; with SF_TANK_CANCONTROL the
// player can man it from its func_tankcontrols
static bool player_touches(entity_t *e, const float *porg, const float *pmins, const float *pmaxs);
static int ents_gen;   // bumped for every map (and saved game) spawn
static int tank_driven = -1;   // the tank the player is using
static int tank_ctrl = -1;     // and the controls they are standing in
static bool tank_fire_held;
static float tank_view[3];
static float angle_wrap(float a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }
static void tank_barrel(entity_t *e, float *pos, float *dir)
{
    float p = e->angles[0] * (float)M_PI / 180, y = e->angles[1] * (float)M_PI / 180;
    float f[3] = {cosf(p) * cosf(y), cosf(p) * sinf(y), -sinf(p)};
    float r[3] = {sinf(y), -cosf(y), 0};
    float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    float bx = ent_float(e, "barrel", 0), by = ent_float(e, "barrely", 0), bz = ent_float(e, "barrelz", 0);
    for (int k = 0; k < 3; k++) pos[k] = e->origin[k] + f[k] * bx - r[k] * by - u[k] * bz;
    memcpy(dir, f, 12);
}
static void tank_fire(entity_t *e, bool by_player)
{
    float pos[3], dir[3];
    tank_barrel(e, pos, dir);
    int kind = !strcasecmp(e->classname, "func_tanklaser") ? 1 : !strcasecmp(e->classname, "func_tankrocket") ? 2 :
               !strcasecmp(e->classname, "func_tankmortar") ? 3 : 0;
    float dmg = ent_float(e, "bullet_damage", 0);
    if (dmg <= 0) {
        int b = (int)ent_float(e, "bullet", 0);
        dmg = kind == 1 ? 30 : kind == 3 ? ent_float(e, "iMagnitude", 100) : skill_get(b == 3 ? "sk_12mm_bullet" : b == 2 ? "sk_9mmAR_bullet" : "sk_9mm_bullet");
        if (dmg <= 0) dmg = 8;
    }
#ifdef BOSSDEBUG
    debugf("tank %s fires (kind %d dmg %.0f) yaw %.0f pitch %.0f\n", e->targetname, kind, dmg, e->angles[1], e->angles[0]);
#endif
    static const float spreads[5] = {0, 0.025f, 0.05f, 0.1f, 0.2f};
    int sp = (int)ent_float(e, "firespread", 0);
    extern void game_tank_fire(int kind, const float *from, const float *dir, float spread, float dmg, bool by_player);
    game_tank_fire(kind, pos, dir, spreads[sp < 0 ? 0 : sp > 4 ? 4 : sp], dmg, by_player);
}
static void tank_turn(entity_t *e, float want_yaw, float want_pitch, float dt)
{
    float yr = ent_float(e, "yawrange", 0), pr = ent_float(e, "pitchrange", 0);
    float yrate = ent_float(e, "yawrate", 30), prate = ent_float(e, "pitchrate", 30);
    float dy = angle_wrap(want_yaw - e->ang1[1]);
    if (yr > 0 && yr < 180) dy = fminf(fmaxf(dy, -yr), yr);
    float dp = angle_wrap(want_pitch - e->ang1[0]);
    dp = fminf(fmaxf(dp, -pr), pr);
    float cy = angle_wrap(e->angles[1] - e->ang1[1]), cp = angle_wrap(e->angles[0] - e->ang1[0]);
    float sy = yrate * dt, spp = prate * dt;
    cy += fminf(fmaxf(dy - cy, -sy), sy);
    cp += fminf(fmaxf(dp - cp, -spp), spp);
    e->angles[1] = e->ang1[1] + cy;
    e->angles[0] = e->ang1[0] + cp;
}
static void tank_think(entity_t *e, int idx, float dt, const float *porg)
{
    float rate = ent_float(e, "firerate", 1);
    if (rate <= 0) rate = 1;
    if (idx == tank_driven) {
        // manned: follow the view; the attack button fires; walking off the controls lets go
        if (tank_ctrl > 0 && entities[tank_ctrl].model > 0) {
            float mn[3], mx[3];
            pm_get_hull(mn, mx);
            if (!player_touches(&entities[tank_ctrl], porg, mn, mx)) { tank_driven = -1; return; }
        }
        tank_turn(e, tank_view[1], tank_view[0], dt * 3);
        if (tank_fire_held && g_time >= e->ltime2) { e->ltime2 = g_time + 1 / rate; tank_fire(e, true); }
        return;
    }
    if (!e->active || !(e->spawnflags & 2) || !player_alive() || pm_noclip_active()) return;
    float pos[3], dir[3];
    tank_barrel(e, pos, dir);
    float eye[3];
    player_get_eye(eye);
    float d[3] = {eye[0] - pos[0], eye[1] - pos[1], eye[2] - 12 - pos[2]};
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float maxr = ent_float(e, "maxRange", 0), minr = ent_float(e, "minRange", 0);
    if (l < 1 || (maxr > 0 && l > maxr) || l < minr) return;
    if (g_time >= e->ltime) {   // line of sight a few times a second
        e->ltime = g_time + 0.25f;
        e->triggered = game_visible(pos, eye);
    }
    if (!e->triggered) return;
    float wy = atan2f(d[1], d[0]) * 180 / (float)M_PI, wp = -asinf(d[2] / l) * 180 / (float)M_PI;
    tank_turn(e, wy, wp, dt);
    float tol = ent_float(e, "yawtolerance", 15);
    bool on = fabsf(angle_wrap(e->angles[1] - wy)) <= (tol > 0 ? tol : 15);
    if (on && g_time >= e->ltime2) {
        e->ltime2 = g_time + 1 / rate;
        tank_fire(e, false);
    }
}
bool ents_tank_driving(void) { return tank_driven >= 0; }
// the glow sprites that are switched on (weapons.c draws them)
float ents_time(void) { return g_time; }
int ents_sprites(int *out, int max)
{
    static int16_t list[512];
    static int nlist, gen = -1, built = -1;
    if (gen != ents_gen || built != num_entities) {
        nlist = 0;
        for (int i = 1; i < num_entities && nlist < 512; i++) if (entities[i].type == ENT_SPRITE) list[nlist++] = i;
        gen = ents_gen; built = num_entities;
    }
    int n = 0;
    for (int k = 0; k < nlist && n < max; k++) {
        entity_t *e = &entities[list[k]];
        if (e->active && !e->removed && (e->sprid >= 0 || e->code == 0 || e->code == 3)) out[n++] = list[k];
    }
    return n;
}
// the player's input while manning a gun (main.c): view angles aim it, attack fires
void ents_tank_input(const float *view, bool fire)
{
    if (tank_driven < 0) return;
    entity_t *e = &entities[tank_driven];
    if (e->removed) { tank_driven = -1; return; }
    memcpy(tank_view, view, 12);
    tank_fire_held = fire;
}
static void tank_controls_use(entity_t *c)
{
    if (tank_driven >= 0) { tank_driven = -1; return; }   // let go
    for (int t = ents_find_targetname(c->target, 1); t >= 0; t = ents_find_targetname(c->target, t + 1))
        if (entities[t].type == ENT_TANK && (entities[t].spawnflags & 32)) {
            tank_driven = t;
            tank_ctrl = (int)(c - entities);
            entities[t].active = false;   // CFuncTank::StartControl stops its own aiming
            snd_emit(t, CHAN_ITEM, "plats/train_use1.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
            return;
        }
}

// CMomentaryRotButton::UpdateTarget: the button's position (0..1) drives its momentary doors; other
// targets are used once when it starts turning
static void momentary_update_targets(entity_t *b, bool first)
{
    if (!b->target[0]) return;
    for (int t = ents_find_targetname(b->target, 1); t >= 0; t = ents_find_targetname(b->target, t + 1)) {
        entity_t *d = &entities[t];
        if (d->type == ENT_MOMENTARY_DOOR) {
            for (int k = 0; k < 3; k++) d->dest[k] = d->pos1[k] + (d->pos2[k] - d->pos1[k]) * b->mpos;
        } else if (first) {
            int oldc = cur_caller;
            cur_caller = (int)(b - entities);
            ent_use(d);
            cur_caller = oldc;
        }
    }
}
static void momentary_set(entity_t *b, float pos, bool first)
{
    b->mpos = fminf(fmaxf(pos, 0), 1);
    for (int k = 0; k < 3; k++) b->angles[k] = b->ang1[k] + b->movedir[k] * b->dist * b->mpos;
    momentary_update_targets(b, first);
}

// CMonsterMaker::MakeMonster: one of "monstertype" at the maker, named "netname", if there is room
// and the live/total limits allow it; fires the maker's target
extern int npc_spawn_monster(const char *classname, const float *origin, float yaw, const char *targetname, int maker);
static void maker_make(entity_t *e)
{
    if (e->maker_count == 0) { e->active = false; return; }
    if (e->maker_maxlive > 0 && e->maker_live >= e->maker_maxlive) return;
    const char *type = ent_value(e, "monstertype");
    if (!type || !type[0]) return;
    if (!strcasecmp(type, "monster_snark")) {   // a snark from a nest: the hunting projectile
        extern int proj_spawn_snark(const float *origin, float yaw);
        if (proj_spawn_snark(e->origin, e->angles[1]) < 0) return;
    } else {
        const char *nn = ent_value(e, "netname");
        if (npc_spawn_monster(type, e->origin, e->angles[1], nn ? nn : "", (int)(e - entities)) < 0) return;
    }
    if (e->maker_count > 0) e->maker_count--;
    if (e->target[0]) fire_entity_target(e);
    if (e->maker_count == 0) e->active = false;
}
// a trigger_teleport's effect on the player without touching it (the Nihilanth's teleport balls call
// the trigger's Touch directly)
bool ents_teleport_player(const char *name)
{
    for (int t = ents_find_targetname(name, 1); t >= 0; t = ents_find_targetname(name, t + 1)) {
        entity_t *e = &entities[t];
        if (e->removed || e->type != ENT_TRIGGER_TELEPORT) continue;
        int d = ents_find_targetname(e->target, 1);
        if (d < 0) return false;
        float o[3], pmins[3], pmaxs[3];
        pm_get_hull(pmins, pmaxs);
        memcpy(o, entities[d].origin, 12);
        o[2] += -pmins[2] + 1;
        pm_set_origin(o);
        float zero[3] = {0, 0, 0};
        pm_set_velocity(zero);
        game_set_view_angles(entities[d].angles);
        ents_player_teleported();
        return true;
    }
    return false;
}

void ents_maker_child_died(int maker)
{
    if (maker > 0 && maker < num_entities && entities[maker].type == ENT_MONSTERMAKER && entities[maker].maker_live > 0)
        entities[maker].maker_live--;
}

static void ent_use(entity_t *e)
{
    if (e->removed) return;
    { extern bool lights_use(entity_t *e); if (lights_use(e)) return; }   // light switches
    if (e->type == ENT_OTHER && !strncasecmp(e->classname, "monster_", 8)) {   // waking a boss
        extern bool npc_used(int ent);
        if (npc_used((int)(e - entities))) return;
    }
    if (!strcasecmp(e->classname, "gibshooter")) {
        // CGibShooter: a burst of human gibs (blood splats + flying chunks here)
        int n = (int)ent_float(e, "m_iGibs", 3);
        float v = ent_float(e, "m_flVelocity", 200);
        extern void game_spawn_gibs(const float *o, int count, bool alien, float speed);
        game_spawn_gibs(e->origin, n > 10 ? 10 : (n < 1 ? 1 : n), false, v * 0.6f);
        return;
    }
    if ((e->type == ENT_BUTTON || e->type == ENT_ROT_BUTTON || e->type == ENT_DOOR || e->type == ENT_DOOR_ROTATING) &&
        !master_ok(e)) return;   // locked by its master (UTIL_IsMasterTriggered)
    switch (e->type) {
    case ENT_DOOR:
    case ENT_DOOR_ROTATING:
        if (e->state == MS_AT_TOP || e->state == MS_GOING_UP) {
            if (e->wait == -1 || (e->spawnflags & 32)) door_go_down(e);
        } else {
            door_go_up(e, true);
        }
        break;
    case ENT_BUTTON:
    case ENT_ROT_BUTTON:
        if (e->state == MS_AT_BOTTOM) {
            snd_emit(e - entities, CHAN_VOICE, button_sound(e->movesnd), 1.0f, ATTN_NORM, PITCH_NORM, NULL);
            e->state = MS_GOING_UP;
            if (e->type == ENT_ROT_BUTTON) move_angular(e, e->ang2);
            else move_linear(e, e->pos2);
        }
        break;
    case ENT_PLAT:
        if (e->state == MS_AT_BOTTOM) { e->state = MS_GOING_UP; move_linear(e, e->pos1); mover_sound_start(e); }
        else if (e->state == MS_AT_TOP) { e->state = MS_GOING_DOWN; move_linear(e, e->pos2); mover_sound_start(e); }
        break;
    case ENT_WALL_TOGGLE:
        e->visible = !e->visible;
        e->solid = e->visible ? SOLID_BSP : SOLID_NOT;
        break;
    case ENT_TRIGGER_RELAY:
    case ENT_TRIGGER_ONCE:
    case ENT_TRIGGER_MULTIPLE:
        fire_entity_target(e);
        break;
    case ENT_MULTI_MANAGER: {
        // CMultiManager: targets fire from its think (never synchronously); while running,
        // further triggers are ignored unless the manager is multithreaded (spawnflag 1)
        if (!(e->spawnflags & 1) && e->nextthink > g_time) break;
        float maxdelay = 0;
        // every key that isn't a standard key is "targetname" "delay"
        const char *p = e->kv;
        while (p && *p && *p != '}') {
            while (*p && *p != '"' && *p != '}') p++;
            if (*p != '"') break;
            const char *k = ++p;
            while (*p && *p != '"') p++;
            int klen = p - k;
            if (*p) p++;
            while (*p && *p != '"') p++;
            if (!*p) break;
            const char *v = ++p;
            while (*p && *p != '"') p++;
            if (*p) p++;
            char key[48];
            if (klen > 47) klen = 47;
            memcpy(key, k, klen); key[klen] = 0;
            if (!strcasecmp(key, "classname") || !strcasecmp(key, "targetname") || !strcasecmp(key, "origin") ||
                !strcasecmp(key, "spawnflags") || !strcasecmp(key, "wait"))
                continue;
            char *hash = strchr(key, '#'); // multi_manager duplicate key syntax
            if (hash) *hash = 0;
            float d = atof(v);
            if (d > maxdelay) maxdelay = d;
            queue_event(key, g_time + d);
        }
        e->nextthink = g_time + maxdelay + 0.001f;
        break;
    }
    case ENT_TRIGGER_CHANGELEVEL:
        request_changelevel(e);
        break;
    case ENT_ROTATING:
        e->active = !e->active;
        break;
    case ENT_TRAIN:
        // CFuncTrain::Use toggles between waiting and moving
        if (e->state == MS_IDLE || e->triggered) { e->triggered = false; train_next(e); }
        else { e->triggered = true; e->active = false; }
        break;
    case ENT_TRACKTRAIN:
        tt_use(e, (int)(e - entities));
        break;
    case ENT_TRACKAUTO:
        trackchange_use(e);
        break;
    case ENT_PATH_TRACK:
        path_track_use(e);
        break;
    case ENT_SCRIPTED:
        npc_script_use((int)(e - entities));   // npc.c: walk to the mark, play, fire target
        break;
    case ENT_CHANGETARGET: {
        const char *nt = ent_value(e, "m_iszNewTarget");
        char newt[48];
        str_copy(newt, nt, sizeof(newt));
        for (int i = ents_find_targetname(e->target, 1); i >= 0; i = ents_find_targetname(e->target, i + 1))
            str_copy(entities[i].target, newt, sizeof(entities[i].target));
        break;
    }
    case ENT_BREAKABLE:
    case ENT_PUSHABLE:
        if (e->type == ENT_BREAKABLE || e->maxhealth > 0) breakable_die(e);
        break;
    case ENT_TRIGGER_HURT:
        e->active = !e->active;          // CBaseTrigger::ToggleUse
        break;
    case ENT_MULTISOURCE:
    case ENT_ENV_FADE:
    case ENT_ENV_MESSAGE:
    case ENT_GAME_TEXT:
    case ENT_CAMERA:
    case ENT_SENTENCE:
    case ENT_ENV_RENDER:
    case ENT_ENDSECTION:
    case ENT_AMBIENT:
    case ENT_SPEAKER:
    case ENT_CDAUDIO:
    case ENT_TARGET_CDAUDIO:
    case ENT_ENV_SHAKE:
    case ENT_WEAPONSTRIP:
    case ENT_ENV_SPARK:
    case ENT_ENV_EXPLOSION:
    case ENT_ENV_SHOOTER:
    case ENT_BEVERAGE:
    case ENT_FUNNEL:
    case ENT_ENV_BLOOD:
        special_use(e);
        break;
    case ENT_GLOBAL_BEAM:
        e->active = !e->active;
        break;
    case ENT_ENV_GLOBAL: {
        // CEnvGlobal::Use: triggermode 0 off, 1 on, 2 dead, 3 toggle
        char g[40];
        str_copy(g, ent_value(e, "globalstate"), sizeof(g));
        int mode = (int)ent_float(e, "triggermode", 0), cur = ents_global_state(g), st;
        if (mode == 3) st = cur == 1 ? 0 : 1;
        else st = mode == 2 ? 2 : mode;
        gstate_set(g, st);
        break;
    }
    case ENT_LOADSAVED: {
        // CRevertSaved: fade out, show the message, then load the last save (start the map again)
        uint8_t c[4];
        parse_color(e, "rendercolor", c, 3);
        c[3] = (uint8_t)ent_float(e, "renderamt", 255);
        hud_fade(c, ent_float(e, "duration", 2), ent_float(e, "holdtime", 2), FFADE_OUT | FFADE_STAYOUT);
        const char *msg = ent_value(e, "message");
        if (msg && msg[0]) hud_show_message(msg);
        extern void game_request_restart(float delay);
        game_request_restart(ent_float(e, "loadtime", 0) + ent_float(e, "duration", 2) + 0.5f);
        break;
    }
    case ENT_PENDULUM:
        e->active = !e->active;
        break;
    case ENT_TANK:
        e->active = !e->active;   // CFuncTank::Use: TankActivate / TankDeactivate
        break;
    case ENT_SPRITE: {
        float sc = ent_float(e, "scale", 1);
        if (sc <= 0) sc = 1;
        if (e->code == 5) { e->active = true; e->sprt0 = g_time; }
        else if (e->code == 1) game_effect_explosion(e->origin, 60 * sc);
        else if (e->code == 2) { extern void game_effect_flash(const float *o, float rad); game_effect_flash(e->origin, 24 * sc); }
        else { e->active = !e->active; if (e->active) e->sprt0 = g_time; }
        break;
    }
    case ENT_MORTAR_FIELD: {
        // CFuncMortarField::FieldUse: shells come down inside the field (m_iCount, m_flSpread)
        if (e->model <= 0) break;
        bmodel_t *bm = &world.bmodels[e->model];
        int count = (int)ent_float(e, "m_iCount", 1);
        float spread = ent_float(e, "m_flSpread", 64);
        float c[3];
        for (int k = 0; k < 3; k++) c[k] = e->origin[k] + (bm->mins[k] + bm->maxs[k]) * 0.5f;
        int ctl = (int)ent_float(e, "m_fControl", 0);
        if (ctl == 1) { float po[3]; pm_get_origin(po); c[0] = po[0]; c[1] = po[1]; }   // at the activator
        extern int proj_spawn_vel(int kind, const float *origin, const float *vel, int owner, int target);
        snd_emit(-1, CHAN_AUTO, "weapons/mortar.wav", 1, ATTN_NONE, PITCH_NORM, c);
        for (int n = 0; n < count && n < 12; n++) {
            float o[3] = {c[0] + ((rand() % 200) - 100) / 100.0f * spread, c[1] + ((rand() % 200) - 100) / 100.0f * spread, e->origin[2] + bm->maxs[2] - 8};
            float v[3] = {0, 0, -900 - rand() % 300};
            proj_spawn_vel(2 /* P_ARGRENADE */, o, v, -1, -1);
        }
        break;
    }
    case ENT_MONSTERMAKER:
        // CMonsterMaker::CyclicUse makes one per use; ToggleUse switches the timed maker on and off
        if (e->spawnflags & 4) maker_make(e);
        else { e->active = !e->active; if (e->active) e->nextthink = g_time; }
        break;
    default:
        break;
    }
}

// ------------------------------------------------------------------ spawning
static enttype_t classify(const char *cn)
{
    static const struct { const char *n; enttype_t t; } tab[] = {
        {"worldspawn", ENT_WORLD}, {"func_wall", ENT_WALL}, {"func_illusionary", ENT_ILLUSIONARY},
        {"func_door", ENT_DOOR}, {"func_water", ENT_DOOR}, {"func_door_rotating", ENT_DOOR_ROTATING},
        {"func_button", ENT_BUTTON}, {"func_rot_button", ENT_ROT_BUTTON}, {"func_plat", ENT_PLAT}, {"func_platrot", ENT_PLAT},
        {"func_train", ENT_TRAIN}, {"func_tracktrain", ENT_TRACKTRAIN}, {"func_rotating", ENT_ROTATING},
        {"func_ladder", ENT_LADDER}, {"func_breakable", ENT_BREAKABLE}, {"func_wall_toggle", ENT_WALL_TOGGLE},
        {"trigger_once", ENT_TRIGGER_ONCE}, {"trigger_multiple", ENT_TRIGGER_MULTIPLE},
        {"trigger_changelevel", ENT_TRIGGER_CHANGELEVEL}, {"trigger_teleport", ENT_TRIGGER_TELEPORT},
        {"trigger_relay", ENT_TRIGGER_RELAY}, {"trigger_auto", ENT_TRIGGER_AUTO},
        {"scripted_sequence", ENT_SCRIPTED}, {"aiscripted_sequence", ENT_SCRIPTED},
        {"trigger_changetarget", ENT_CHANGETARGET}, {"multi_manager", ENT_MULTI_MANAGER},
        {"trigger_push", ENT_TRIGGER_PUSH}, {"trigger_hurt", ENT_TRIGGER_HURT},
        {"func_pushable", ENT_PUSHABLE}, {"func_conveyor", ENT_CONVEYOR}, {"momentary_door", ENT_MOMENTARY_DOOR},
        {"func_plat_toggle", ENT_PLAT}, {"func_trackautochange", ENT_TRACKAUTO}, {"func_trackchange", ENT_TRACKAUTO},
        {"func_monsterclip", ENT_INVISIBLE_BRUSH}, {"func_traincontrols", ENT_INVISIBLE_BRUSH},
        {"path_track", ENT_PATH_TRACK}, {"multisource", ENT_MULTISOURCE}, {"env_fade", ENT_ENV_FADE},
        {"env_message", ENT_ENV_MESSAGE}, {"game_text", ENT_GAME_TEXT}, {"trigger_camera", ENT_CAMERA},
        {"scripted_sentence", ENT_SENTENCE}, {"env_render", ENT_ENV_RENDER}, {"trigger_endsection", ENT_ENDSECTION},
        {"func_guntarget", ENT_WALL}, {"func_mortar_field", ENT_MORTAR_FIELD},
        {"func_tank", ENT_TANK}, {"func_tankcontrols", ENT_TANKCONTROLS}, {"func_recharge", ENT_RECHARGE},
        {"func_healthcharger", ENT_HEALTHCHARGER}, {"momentary_rot_button", ENT_MOMENTARY_BUTTON}, {"func_friction", ENT_FRICTION},
        {"trigger_autosave", ENT_AUTOSAVE}, {"trigger_cdaudio", ENT_CDAUDIO}, {"target_cdaudio", ENT_TARGET_CDAUDIO},
        {"ambient_generic", ENT_AMBIENT}, {"speaker", ENT_SPEAKER}, {"env_shake", ENT_ENV_SHAKE},
        {"player_weaponstrip", ENT_WEAPONSTRIP}, {"env_beam", ENT_GLOBAL_BEAM}, {"env_laser", ENT_GLOBAL_BEAM}, {"env_spark", ENT_ENV_SPARK}, {"monstermaker", ENT_MONSTERMAKER}, {"env_global", ENT_ENV_GLOBAL}, {"env_sprite", ENT_SPRITE}, {"env_glow", ENT_SPRITE}, {"env_shooter", ENT_ENV_SHOOTER}, {"env_beverage", ENT_BEVERAGE}, {"env_funnel", ENT_FUNNEL}, {"env_blood", ENT_ENV_BLOOD}, {"cycler_sprite", ENT_SPRITE}, {"player_loadsaved", ENT_LOADSAVED}, {"func_pendulum", ENT_PENDULUM}, {"env_explosion", ENT_ENV_EXPLOSION},
        {"trigger_monsterjump", ENT_INVISIBLE_BRUSH}, {"trigger_gravity", ENT_TRIGGER_GRAVITY},
        {"trigger_transition", ENT_INVISIBLE_BRUSH}, {"func_vehiclecontrols", ENT_INVISIBLE_BRUSH},
        {"trigger", ENT_INVISIBLE_BRUSH},
        {"env_bubbles", ENT_INVISIBLE_BRUSH}, {"func_tankmortar", ENT_TANK}, {"func_tanklaser", ENT_TANK},
        {"func_tankrocket", ENT_TANK},
    };
    for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (!strcasecmp(cn, tab[i].n)) return tab[i].t;
    return ENT_OTHER;
}

static void spawn_entity(entity_t *e)
{
    const char *v;
    str_copy(e->classname, ent_value(e, "classname"), sizeof(e->classname));
    str_copy(e->targetname, ent_value(e, "targetname"), sizeof(e->targetname));
    str_copy(e->target, ent_value(e, "target"), sizeof(e->target));
    e->type = classify(e->classname);
    v = ent_value(e, "model");
    e->model = (v && v[0] == '*') ? atoi(v + 1) : -1;
    if (e->model >= world.nummodels) e->model = -1;
    ent_vec(e, "origin", e->origin);
    ent_vec(e, "angles", e->angles);
    if ((v = ent_value(e, "angle"))) {
        float a = atof(v);
        if (a == -1 || a == -2) { e->angles[0] = 0; e->angles[1] = a; e->angles[2] = 0; }
        else { e->angles[1] = a; }
    }
    e->spawnflags = (int)ent_float(e, "spawnflags", 0);
    e->speed = ent_float(e, "speed", 0);
    e->wait = ent_float(e, "wait", 0);
    e->lip = ent_float(e, "lip", 0);
    e->delay = ent_float(e, "delay", 0);
    e->dist = ent_float(e, "distance", 90);
    e->health = ent_float(e, "health", 0);
    e->rendermode = (int)ent_float(e, "rendermode", 0);
    e->renderamt = (int)ent_float(e, "renderamt", 255);
    e->renderfx = (int)ent_float(e, "renderfx", 0);
    e->movesnd = (int)ent_float(e, "movesnd", 0);
    e->stopsnd = (int)ent_float(e, "stopsnd", 0);
    e->visible = e->model > 0;
    e->solid = e->model > 0 ? SOLID_BSP : SOLID_NOT;
    e->state = MS_IDLE;
    e->triggered = true;
    memcpy(e->base_origin, e->origin, 12);

    // render modes that are invisible in practice
    if (e->rendermode != 0 && e->renderamt == 0 && e->rendermode != 4) e->visible = false;

    float size[3];
    switch (e->type) {
    case ENT_WORLD:
        e->visible = false; e->solid = SOLID_NOT; // drawn separately
        break;
    case ENT_ILLUSIONARY:
        e->solid = SOLID_NOT;
        break;
    case ENT_WATER:
    case ENT_DOOR:
        if (e->type == ENT_WATER) {
            e->contents = (int)ent_float(e, "skin", CONTENTS_WATER);
            if (e->contents == 0) e->contents = CONTENTS_WATER;
            e->solid = SOLID_NOT;
        } else {
            v = ent_value(e, "skin");
            if (v && atoi(v) < 0) { e->contents = atoi(v); e->solid = SOLID_NOT; }
            // func_water is a CBaseDoor: it moves like one (the sewer valve raising the water)
            if (!strcasecmp(e->classname, "func_water")) {
                if (e->contents >= 0) e->contents = CONTENTS_WATER;
                e->solid = SOLID_NOT;
            }
        }
        set_movedir(e);
        if (e->speed == 0) e->speed = 100;
        if (e->spawnflags & 8) e->solid = SOLID_NOT; // passable
        model_size(e, size);
        memcpy(e->pos1, e->origin, 12);
        {
            float d = fabsf(e->movedir[0] * (size[0] - 2)) + fabsf(e->movedir[1] * (size[1] - 2)) +
                      fabsf(e->movedir[2] * (size[2] - 2)) - e->lip;
            for (int i = 0; i < 3; i++) e->pos2[i] = e->pos1[i] + e->movedir[i] * d;
        }
        if (e->spawnflags & 1) { // start open
            memcpy(e->origin, e->pos2, 12);
            memcpy(e->pos2, e->pos1, 12);
            memcpy(e->pos1, e->origin, 12);
        }
        e->state = MS_AT_BOTTOM;
        break;
    case ENT_DOOR_ROTATING:
        // doors.cpp CRotDoor::Spawn
        memset(e->movedir, 0, 12);
        if (e->spawnflags & 64) e->movedir[2] = 1;
        else if (e->spawnflags & 128) e->movedir[0] = 1;
        else e->movedir[1] = 1;
        if (e->spawnflags & 2) for (int i = 0; i < 3; i++) e->movedir[i] = -e->movedir[i];
        memcpy(e->ang1, e->angles, 12);
        for (int i = 0; i < 3; i++) e->ang2[i] = e->angles[i] + e->movedir[i] * e->dist;
        if (e->speed == 0) e->speed = 100;
        if (e->spawnflags & 8) e->solid = SOLID_NOT;
        if (e->spawnflags & 1) {
            memcpy(e->angles, e->ang2, 12);
            float t[3];
            memcpy(t, e->ang2, 12); memcpy(e->ang2, e->ang1, 12); memcpy(e->ang1, t, 12);
            for (int i = 0; i < 3; i++) e->movedir[i] = -e->movedir[i];
        }
        e->state = MS_AT_BOTTOM;
        break;
    case ENT_BUTTON:
        set_movedir(e);
        if (e->speed == 0) e->speed = 40;
        if (e->wait == 0) e->wait = 1;
        if (e->lip == 0) e->lip = 4;
        e->movesnd = (int)ent_float(e, "sounds", 0);
        model_size(e, size);
        memcpy(e->pos1, e->origin, 12);
        {
            float d = fabsf(e->movedir[0] * (size[0] - 2)) + fabsf(e->movedir[1] * (size[1] - 2)) +
                      fabsf(e->movedir[2] * (size[2] - 2)) - e->lip;
            for (int i = 0; i < 3; i++) e->pos2[i] = e->pos1[i] + e->movedir[i] * d;
            // "don't move" buttons
            if (fabsf(d) < 1 || (e->spawnflags & 1)) memcpy(e->pos2, e->pos1, 12);
        }
        e->state = MS_AT_BOTTOM;
        break;
    case ENT_ROT_BUTTON:
        memset(e->movedir, 0, 12);
        if (e->spawnflags & 64) e->movedir[2] = 1;
        else if (e->spawnflags & 128) e->movedir[0] = 1;
        else e->movedir[1] = 1;
        if (e->spawnflags & 2) for (int i = 0; i < 3; i++) e->movedir[i] = -e->movedir[i];
        memcpy(e->ang1, e->angles, 12);
        for (int i = 0; i < 3; i++) e->ang2[i] = e->angles[i] + e->movedir[i] * e->dist;
        if (e->speed == 0) e->speed = 40;
        if (e->wait == 0) e->wait = 1;
        e->movesnd = (int)ent_float(e, "sounds", 0);
        e->state = MS_AT_BOTTOM;
        break;
    case ENT_PLAT: {
        // plats.cpp: pos1 = top (as placed), pos2 = lowered by height
        if (e->speed == 0) e->speed = 150;
        model_size(e, size);
        float h = ent_float(e, "height", 0);
        if (h == 0) h = size[2] - 8;
        memcpy(e->pos1, e->origin, 12);
        memcpy(e->pos2, e->origin, 12);
        e->pos2[2] -= h;
        if (!e->targetname[0]) { memcpy(e->origin, e->pos2, 12); e->state = MS_AT_BOTTOM; }
        else e->state = MS_AT_TOP;
        // func_platrot: turns about yaw while it travels (m_end at the top, m_start at the bottom)
        memset(e->ang2, 0, 12);
        if (!strcasecmp(e->classname, "func_platrot")) {
            e->ang2[1] = ent_float(e, "rotation", 0);
            e->angles[1] = e->state == MS_AT_TOP ? e->ang2[1] : 0;
        }
        break;
    }
    case ENT_ROTATING:
        if (e->speed == 0) e->speed = 100;
        memset(e->avelocity, 0, 12);
        if (e->spawnflags & 4) e->avelocity[2] = 1;       // Z axis -> roll
        else if (e->spawnflags & 8) e->avelocity[0] = 1;  // X axis -> pitch
        else e->avelocity[1] = 1;
        if (e->spawnflags & 2) for (int i = 0; i < 3; i++) e->avelocity[i] = -e->avelocity[i];
        e->active = (e->spawnflags & 1) != 0; // start on
        if (e->spawnflags & 64) e->solid = SOLID_NOT;
        break;
    case ENT_LADDER:
        e->contents = -16; // CONTENTS_LADDER
        e->solid = SOLID_NOT;
        e->visible = false;
        break;
    case ENT_PUSHABLE:
        // CPushable: breakable only with SF_PUSH_BREAKABLE; it falls to the floor or floats in water
        e->maxhealth = (e->spawnflags & 128) ? e->health : 0;
        if (e->model > 0) pushable_settle_pending = true;
        if (num_pushables < MAX_PUSHABLES) pushable_list[num_pushables] = (uint16_t)(e - entities);
        num_pushables++;
        break;
    case ENT_BREAKABLE:
        // func_breakable.cpp: health 0 = unbreakable by damage (SF_BREAK_TRIGGER_ONLY is 1)
        e->maxhealth = e->health;
        break;
    case ENT_WALL_TOGGLE:
        if (e->spawnflags & 1) { e->visible = false; e->solid = SOLID_NOT; } // starts invisible
        break;
    case ENT_TRIGGER_ONCE:
    case ENT_TRIGGER_MULTIPLE:
    case ENT_TRIGGER_CHANGELEVEL:
    case ENT_TRIGGER_TELEPORT:
    case ENT_TRIGGER_PUSH:
    case ENT_TRIGGER_HURT:
    case ENT_INVISIBLE_BRUSH:
        e->visible = false;
        e->solid = SOLID_NOT;
        if (e->type == ENT_TRIGGER_MULTIPLE && e->wait == 0) e->wait = 0.2f;
        if (e->type == ENT_TRIGGER_HURT) e->active = !(e->spawnflags & 2);   // SF_TRIGGER_HURT_START_OFF
        if (e->type == ENT_TRIGGER_PUSH) {
            set_movedir(e);
            if (e->speed == 0) e->speed = 100;
            e->active = !(e->spawnflags & 2);
        }
        break;
    case ENT_CONVEYOR:
        e->solid = (e->spawnflags & 2) ? SOLID_NOT : SOLID_BSP;
        break;
    case ENT_TRAIN:
        if (e->speed == 0) e->speed = 100;
        if (e->spawnflags & 8) e->solid = SOLID_NOT;
        {   // "skin" < 0 is the brush's contents: the rising water in c1a1b is a func_train of water
            const char *sk = ent_value(e, "skin");
            if (sk && atoi(sk) < 0) { e->contents = atoi(sk); e->solid = SOLID_NOT; }
        }
        e->path = -1;
        e->state = MS_IDLE;
        e->triggered = false;
        e->nextthink = 0.1f; // CFuncTrain::Activate on the first think
        break;
    case ENT_TRACKAUTO: {
        // plats.cpp CFuncPlat::Setup + CFuncPlatRot::SetupRotation + CFuncTrackChange::Spawn
        if (e->speed == 0) e->speed = 150;
        model_size(e, size);
        float h = ent_float(e, "height", 0);
        memset(e->angles, 0, 12);
        memcpy(e->pos1, e->origin, 12);
        memcpy(e->pos2, e->origin, 12);
        e->pos2[2] = h != 0 ? e->origin[2] - h : e->origin[2] - size[2] + 8;
        if (e->spawnflags & SF_TRACK_DONT_MOVE) e->pos2[2] = e->origin[2];
        float rot = ent_float(e, "rotation", 0);
        memset(e->ang1, 0, 12);                      // m_start
        memset(e->ang2, 0, 12);
        e->ang2[1] = rot;                            // m_end (yaw axis)
        if (e->spawnflags & SF_TRACK_STARTBOTTOM) {
            memcpy(e->origin, e->pos2, 12);
            memcpy(e->angles, e->ang1, 12);
            e->state = MS_AT_BOTTOM;
            e->targetstate = MS_AT_TOP;
        } else {
            memcpy(e->origin, e->pos1, 12);
            memcpy(e->angles, e->ang2, 12);
            e->state = MS_AT_TOP;
            e->targetstate = MS_AT_BOTTOM;
        }
        e->tracktop = e->trackbottom = -1;
        e->train = -1;
        e->code = TRAIN_SAFE;
        break;
    }
    case ENT_TRACKTRAIN: {
        if (e->speed == 0) e->speed = 100;   // m_speed
        e->curspeed = 0;
        e->height = ent_float(e, "height", 0);
        e->length = ent_float(e, "wheels", 0);
        if (e->length == 0) e->length = 100;
        if (e->spawnflags & 8) e->solid = SOLID_NOT;
        e->path = -1;                        // CFuncTrackTrain::Find on the first think
        // cache the placed origin for the train controls; default control area: the whole train
        memcpy(e->oldorigin, e->origin, 12);
        bmodel_t *bm = &world.bmodels[e->model > 0 ? e->model : 0];
        memcpy(e->ctlmins, bm->mins, 12);
        memcpy(e->ctlmaxs, bm->maxs, 12);
        e->ctlmaxs[2] += 72;
        break;
    }
    case ENT_PATH_TRACK:
        e->visible = false;
        e->solid = SOLID_NOT;
        break;
    case ENT_ENDSECTION:
    case ENT_CDAUDIO:
        e->visible = false;
        e->solid = SOLID_NOT;
        break;
    case ENT_AMBIENT:
        e->active = false;
        if (!(e->spawnflags & 16) && !(e->spawnflags & 32)) { e->active = true; e->nextthink = 0.1f; }
        break;
    case ENT_SPEAKER:
        e->nextthink = (e->spawnflags & 1) ? 0 : 5.0f + (rand() % 100) / 10.0f;
        break;
    case ENT_RECHARGE:
        e->juice = skill_get("sk_suitcharger");
        break;
    case ENT_HEALTHCHARGER:
        e->juice = skill_get("sk_healthcharger");
        break;
    case ENT_GLOBAL_BEAM:
        e->active = (e->spawnflags & 1) || !e->targetname[0];
        break;
    case ENT_ENV_GLOBAL: {
        // CEnvGlobal::Spawn: SF_GLOBAL_SET puts the initial state in if the state isn't known yet
        char g[40];
        str_copy(g, ent_value(e, "globalstate"), sizeof(g));
        if ((e->spawnflags & 1) && g[0] && ents_global_state(g) < 0) gstate_set(g, (int)ent_float(e, "initialstate", 0));
        e->visible = false; e->solid = SOLID_NOT;
        break;
    }
    case ENT_TRIGGER_GRAVITY:
    case ENT_FRICTION:
    case ENT_AUTOSAVE:
    case ENT_MORTAR_FIELD:
        e->visible = false;
        e->solid = SOLID_NOT;
        break;
    case ENT_MOMENTARY_DOOR:
        // CMomentaryDoor: a door whose position follows a momentary_rot_button
        set_movedir(e);
        if (e->speed == 0) e->speed = 100;
        model_size(e, size);
        memcpy(e->pos1, e->origin, 12);
        {
            float d = fabsf(e->movedir[0] * (size[0] - 2)) + fabsf(e->movedir[1] * (size[1] - 2)) +
                      fabsf(e->movedir[2] * (size[2] - 2)) - e->lip;
            for (int i = 0; i < 3; i++) e->pos2[i] = e->pos1[i] + e->movedir[i] * d;
        }
        memcpy(e->dest, e->origin, 12);
        break;
    case ENT_MOMENTARY_BUTTON:
    case ENT_PENDULUM:
        // CMomentaryRotButton / CPendulum: turn about an axis (CBaseToggle::AxisDir)
        memset(e->movedir, 0, 12);
        if (e->spawnflags & 64) e->movedir[2] = 1;
        else if (e->spawnflags & 128) e->movedir[0] = 1;
        else e->movedir[1] = 1;
        if (e->type == ENT_MOMENTARY_BUTTON && (e->spawnflags & 2)) for (int i = 0; i < 3; i++) e->movedir[i] = -e->movedir[i];
        memcpy(e->ang1, e->angles, 12);
        if (e->speed == 0) e->speed = e->type == ENT_PENDULUM ? 100 : 100;
        e->mpos = 0; e->mdir = -1; e->mlast = -10;
        if (e->type == ENT_MOMENTARY_BUTTON && (e->spawnflags & 16)) e->solid = SOLID_NOT;
        if (e->type == ENT_PENDULUM) e->active = (e->spawnflags & 1) || !e->targetname[0];
        break;
    case ENT_TANK:
        // CFuncTank::Spawn: the base direction it turns about, active with SF_TANK_ACTIVE
        memcpy(e->ang1, e->angles, 12);
        e->active = (e->spawnflags & 1) != 0;
        e->ltime2 = 0;
        break;
    case ENT_TANKCONTROLS:
        e->visible = false;
        e->solid = SOLID_NOT;
        break;
    case ENT_SPRITE: {
        // env_sprite / env_glow: glows and flares stay up (SF_SPRITE_STARTON, or no name); the
        // explosion / flash kinds play once when triggered (SF_SPRITE_ONCE)
        const char *m = ent_value(e, "model");
        char nm[48] = "";
        if (m) for (int k = 0; m[k] && k < 47; k++) nm[k] = tolower((unsigned char)m[k]), nm[k + 1] = 0;
        {   // the PC sprite itself (spr/index.txt), animated at "framerate"
            extern int fxspr_find(const char *name);
            const char *b = strrchr(nm, '/');
            char base[32];
            str_copy(base, b ? b + 1 : nm, sizeof(base));
            char *dot = strrchr(base, '.');
            if (dot) *dot = 0;
            e->sprid = fxspr_find(base);
            e->sprt0 = 0;
        }
        if (strstr(nm, "flare1") || strstr(nm, "flare3") || strstr(nm, "glow0") || strstr(nm, "flare2")) e->code = 0;
        else if (strstr(nm, "xplo") || strstr(nm, "blast") || strstr(nm, "explode")) e->code = 1;
        else if (strstr(nm, "xflare") || strstr(nm, "xspark") || strstr(nm, "muzzleflash") || strstr(nm, "stmbal")) e->code = 2;
        else if (strstr(nm, "enter1") || strstr(nm, "exit1")) e->code = 3;
        else e->code = 4;   // smoke and other animated sheets: not drawn
        bool once = !strcasecmp(e->classname, "env_sprite") && (e->spawnflags & 2);
        if (once && e->code < 1) e->code = 2;
        if (e->sprid >= 0) e->code = once ? 5 : 0;   // 5: play the animation once when used
        e->active = !once && (!strcasecmp(e->classname, "env_glow") || !e->targetname[0] || (e->spawnflags & 1));
        e->visible = false; e->solid = SOLID_NOT; e->leaf = 0;
        break;
    }
    case ENT_MONSTERMAKER:
        e->maker_count = (int)ent_float(e, "monstercount", -1);
        e->maker_maxlive = (int)ent_float(e, "m_imaxlivechildren", -1);
        e->maker_live = 0;
        e->delay = ent_float(e, "delay", 1);
        if (e->delay <= 0) e->delay = 1;
        // SF_MONSTERMAKER_START_ON, or no name to be triggered by
        e->active = !(e->spawnflags & 4) && ((e->spawnflags & 1) || !e->targetname[0]);
        e->nextthink = e->active ? g_time + e->delay : 0;
        break;
    case ENT_ENV_SPARK:
        e->active = !(e->spawnflags & 32) || (e->spawnflags & 64);
        e->nextthink = e->active ? 0.1f + (rand() % 100) / 100.0f * ent_float(e, "MaxDelay", 0) : 0;
        break;
    default:
        break;
    }
    if (e->type == ENT_OTHER && e->model > 0) {
        // unknown brush entity: draw and collide, as most func_* are solid walls
        e->visible = true;
    }
}

void ents_spawn_all(void)
{
    ents_gen++;
    tank_driven = -1;
    num_msrc = 0;
    memset(beam_cache, 0, sizeof(beam_cache));
    memset(master_cache, 0, sizeof(master_cache));
    num_pushables = 0;
    memset(entities, 0, sizeof(entities));
    num_entities = 0;
    driving = -1;
    num_events = 0;
    g_time = 0;
    const char *p = world.entities;
    while (*p && num_entities < MAX_ENTITIES) {
        while (*p && *p != '{') p++;
        if (!*p) break;
        entity_t *e = &entities[num_entities++];
        e->kv = p + 1;
        // skip to the end of this block (values never contain braces in HL maps)
        while (*p && *p != '}') {
            if (*p == '"') { p++; while (*p && *p != '"') p++; }
            if (*p) p++;
        }
        spawn_entity(e);
    }
    path_link_all();
    for (int i = 1; i < num_entities; i++) {
        entity_t *e = &entities[i];
        if (!strcasecmp(e->classname, "func_traincontrols") && e->target[0]) {
            // CFuncTrainControls::Find -> CFuncTrackTrain::SetControls
            int t = ents_find_targetname(e->target, 1);
            if (t >= 0 && entities[t].type == ENT_TRACKTRAIN && e->model > 0) {
                bmodel_t *bm = &world.bmodels[e->model];
                for (int k = 0; k < 3; k++) {
                    float off = e->origin[k] - entities[t].oldorigin[k];
                    entities[t].ctlmins[k] = bm->mins[k] + off;
                    entities[t].ctlmaxs[k] = bm->maxs[k] + off;
                }
            }
        }
        if (e->type == ENT_MULTISOURCE) {
            int n = 0;
            msrc_t *m = num_msrc < MAX_MSOURCES ? &msrc[num_msrc++] : NULL;
            if (m) { memset(m, 0, sizeof(*m)); m->ent = i; }
            for (int j = 1; j < num_entities; j++)
                if (entities[j].target[0] && !strcasecmp(entities[j].target, e->targetname)) {
                    if (m && n < 24) m->src[n] = j;
                    n++;
                }
            if (m) m->n = n > 24 ? 24 : n;
            e->code = n;          // sources needed (0: only the global state decides)
        }
        if (e->type == ENT_SENTENCE && !e->targetname[0]) e->nextthink = 1.0f;   // starts by itself
    }
    debugf("spawned %d entities\n", num_entities);
}

// ------------------------------------------------------------------ per-frame
static bool player_touches(entity_t *e, const float *porg, const float *pmins, const float *pmaxs)
{
    bmodel_t *bm = &world.bmodels[e->model];
    for (int i = 0; i < 3; i++) {
        if (porg[i] + pmins[i] > e->origin[i] + bm->maxs[i]) return false;
        if (porg[i] + pmaxs[i] < e->origin[i] + bm->mins[i]) return false;
    }
    return true;
}

// func_pushable riding on a lift / train (the specimen cart in Anomalous Materials)
static void carry_pushables(entity_t *m, const float *moved)
{
    if (m->model <= 0) return;
    bmodel_t *mb = &world.bmodels[m->model];
    for (int i = 1; i < num_entities; i++) {
        entity_t *p = &entities[i];
        if (p->type != ENT_PUSHABLE || p->removed || p->model <= 0 || p == m) continue;
        bmodel_t *pb = &world.bmodels[p->model];
        float bottom = p->origin[2] + pb->mins[2], top = m->origin[2] - moved[2] + mb->maxs[2];
        if (fabsf(bottom - top) > 4) continue;
        if (p->origin[0] + pb->maxs[0] < m->origin[0] + mb->mins[0] || p->origin[0] + pb->mins[0] > m->origin[0] + mb->maxs[0] ||
            p->origin[1] + pb->maxs[1] < m->origin[1] + mb->mins[1] || p->origin[1] + pb->mins[1] > m->origin[1] + mb->maxs[1]) continue;
        for (int k = 0; k < 3; k++) p->origin[k] += moved[k];
    }
}

// the player walking into a func_pushable shoves it along (CPushable::Move)
static void push_pushable(entity_t *p, const float *porg, const float *pmins, const float *pmaxs, float dt)
{
    float mn[3], mx[3];
    for (int k = 0; k < 3; k++) { mn[k] = pmins[k] - 4; mx[k] = pmaxs[k] + 4; }
    mn[2] += 8;   // standing on top doesn't push
    if (!player_touches(p, porg, mn, mx)) return;
    float v[3];
    pm_get_velocity(v);
    // pushing head-on, the cart stops the player dead: use the wished move instead (CPushable::Move
    // uses the player's speed toward it; here a blocked player still shoves at walking pace)
    if (v[0] * v[0] + v[1] * v[1] < 400 && (g_wish_move[0] != 0 || g_wish_move[1] != 0)) {
        float l = sqrtf(g_wish_move[0] * g_wish_move[0] + g_wish_move[1] * g_wish_move[1]);
        float sp = l > 1 ? 120 : 120 * l;
        v[0] = g_wish_move[0] / (l > 0 ? l : 1) * sp;
        v[1] = g_wish_move[1] / (l > 0 ? l : 1) * sp;
    }
    bmodel_t *pb = &world.bmodels[p->model];
    float c[2] = {p->origin[0] + (pb->mins[0] + pb->maxs[0]) * 0.5f - porg[0], p->origin[1] + (pb->mins[1] + pb->maxs[1]) * 0.5f - porg[1]};
    if (v[0] * c[0] + v[1] * c[1] <= 0) return;   // moving away
    float d[3] = {v[0] * dt * 0.9f, v[1] * dt * 0.9f, 0};
    // check the move against the world with the cart's box
    float o[3] = {p->origin[0], p->origin[1], p->origin[2] + 1}, to[3] = {o[0] + d[0], o[1] + d[1], o[2]};
    gtrace_t tr;
    game_trace(o, to, pb->mins, pb->maxs, TR_WORLD_ONLY, -1, &tr);
    if (tr.startsolid) { for (int k = 0; k < 2; k++) p->origin[k] += d[k]; return; }
    p->origin[0] = tr.endpos[0];
    p->origin[1] = tr.endpos[1];
}

// pushables come to rest on whatever is below them -- the world or a brush entity such as the
// specimen cart's lift -- once every entity exists; in water/slime they float partly sunk
static void settle_pushables(void)
{
    pushable_settle_pending = false;
    for (int i = 1; i < num_entities; i++) {
        entity_t *e = &entities[i];
        if (e->type != ENT_PUSHABLE || e->removed || e->model <= 0) continue;
        bmodel_t *pb = &world.bmodels[e->model];
        float o[3] = {e->origin[0], e->origin[1], e->origin[2] + 1}, to[3] = {o[0], o[1], o[2] - 512};
        gtrace_t tr;
        int sol = e->solid;
        e->solid = SOLID_NOT;   // don't hit itself
        game_trace_begin_frame();
        game_trace(o, to, pb->mins, pb->maxs, TR_IGNORE_MONSTERS | TR_IGNORE_PLAYER, -1, &tr);
        e->solid = sol;
        if (tr.startsolid || tr.fraction >= 1) continue;
        // water between here and the floor: float at the surface instead
        float hgt = pb->maxs[2] - pb->mins[2];
        float cx = e->origin[0] + (pb->mins[0] + pb->maxs[0]) * 0.5f, cy = e->origin[1] + (pb->mins[1] + pb->maxs[1]) * 0.5f;
        float z;
        for (z = e->origin[2]; z > tr.endpos[2]; z -= 4) {
            float p[3] = {cx, cy, z + pb->mins[2]};
            int c = bsp_point_contents(p);
            if (c <= -3 && c >= -5) break;
        }
        e->origin[2] = z > tr.endpos[2] ? z - hgt * 0.4f : tr.endpos[2];
    }
}

static void mover_step(entity_t *e, int idx, float dt)
{
    float *cur = e->angular ? e->angles : e->origin;
    float delta[3], dist2 = 0;
    for (int i = 0; i < 3; i++) { delta[i] = e->dest[i] - cur[i]; dist2 += delta[i] * delta[i]; }
    float dist = sqrtf(dist2);
    float step = e->speed * dt;
    float old[3];
    memcpy(old, cur, 12);
    bool done = false;
    if (dist <= step || dist < 0.001f) { memcpy(cur, e->dest, 12); done = true; }
    else for (int i = 0; i < 3; i++) cur[i] += delta[i] / dist * step;

    if (e->solid == SOLID_BSP) {
        float porg[3];
        pm_get_origin(porg);
        float moved[3];
        for (int i = 0; i < 3; i++) moved[i] = cur[i] - old[i];
        if (!e->angular) carry_pushables(e, moved);
        bool riding = pm_entity_for_physent(pm_get_groundent()) == idx;
        if (!e->angular && riding) {
            float np[3] = {porg[0] + moved[0], porg[1] + moved[1], porg[2] + moved[2]};
            if (moved[2] > 0) np[2] += 0.01f;
            memcpy(porg, np, 12);
            pm_set_origin(np);
        }
        if (pm_player_in_entity(idx, porg)) {
            // try pushing the player along with the mover (SV_Push)
            float np[3] = {porg[0] + moved[0], porg[1] + moved[1], porg[2] + moved[2]};
            if (!e->angular && pm_position_free(np)) {
                pm_set_origin(np);
            } else {
                // blocked: undo, doors reverse (CBaseDoor::Blocked)
                memcpy(cur, old, 12);
                if ((e->type == ENT_DOOR || e->type == ENT_DOOR_ROTATING) && e->wait >= 0) {
                    if (e->state == MS_GOING_DOWN) door_go_up(e, false);
                    else if (e->state == MS_GOING_UP) door_go_down(e);
                }
                return;
            }
        }
    }
    if (done) {
        e->active = false;
        mover_reached(e);
    }
}


// ------------------------------------------------------------------ trains (plats.cpp)
static void model_center(entity_t *e, float *c)
{
    bmodel_t *bm = &world.bmodels[e->model];
    for (int i = 0; i < 3; i++) c[i] = (bm->mins[i] + bm->maxs[i]) * 0.5f;
}

// CFuncTrain::Next - head for the next path_corner
static void train_next(entity_t *e)
{
    int t = ents_find_targetname(e->target, 1);
    if (t < 0) { e->active = false; return; }
    entity_t *corner = &entities[t];
    if (e->path >= 0 && entities[e->path].speed != 0) e->speed = entities[e->path].speed;
    e->path = t;
    str_copy(e->target, corner->target, sizeof(e->target));
    float c[3], dest[3];
    model_center(e, c);
    for (int i = 0; i < 3; i++) dest[i] = corner->origin[i] - c[i];
    if (corner->spawnflags & 2) { // SF_CORNER_TELEPORT
        memcpy(e->origin, dest, 12);
        e->state = MS_GOING_UP;
        e->active = false;
        e->nextthink = g_time + 0.01f;
        return;
    }
    e->state = MS_GOING_UP;
    move_linear(e, dest);
    mover_sound_start(e);
}

// CFuncTrain::Wait - arrived at a corner
static void train_wait(entity_t *e)
{
    entity_t *corner = e->path >= 0 ? &entities[e->path] : NULL;
    if (corner) {
        const char *msg = ent_value(corner, "message");
        if (msg) { char m[48]; str_copy(m, msg, 48); queue_fire(m, 0); }
        if ((corner->spawnflags & 1) || e->triggered) { e->triggered = true; e->state = MS_AT_TOP; mover_sound_stop(e); return; } // wait for retrigger
        float w = corner->wait;
        if (w == -1) { e->state = MS_AT_TOP; mover_sound_stop(e); return; }
        if (w != 0) { e->state = MS_AT_TOP; e->nextthink = g_time + w; mover_sound_stop(e); return; }
    }
    train_next(e);
}

// ------------------------------------------------------------------ path_track (pathcorner.cpp CPathTrack)

static int path_next(int p)
{
    entity_t *e = &entities[p];
    if (e->palt >= 0 && (e->spawnflags & SF_PATH_ALTERNATE) && !(e->spawnflags & SF_PATH_ALTREVERSE)) return e->palt;
    return e->pnext;
}

static int path_prev(int p)
{
    entity_t *e = &entities[p];
    if (e->palt >= 0 && (e->spawnflags & SF_PATH_ALTERNATE) && (e->spawnflags & SF_PATH_ALTREVERSE)) return e->palt;
    return e->pprev;
}

static int path_valid(int p, int test)
{
    if (p < 0) return -1;
    if (test && (entities[p].spawnflags & SF_PATH_DISABLED)) return -1;
    return p;
}

static void path_project(int start, int end, float *origin, float dist)
{
    if (start < 0 || end < 0) return;
    float d[3], l = 0;
    for (int k = 0; k < 3; k++) { d[k] = entities[end].origin[k] - entities[start].origin[k]; l += d[k] * d[k]; }
    l = sqrtf(l);
    if (l < 0.001f) return;
    for (int k = 0; k < 3; k++) origin[k] = entities[end].origin[k] + d[k] / l * dist;
}

static float vlen(const float *v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// CPathTrack::LookAhead
static int path_look_ahead(int start, float *origin, float dist, int move)
{
    int cur = start;
    float orig_dist = dist;
    float pos[3];
    memcpy(pos, origin, 12);
    int guard = 0;
    if (dist < 0) {
        dist = -dist;
        while (dist > 0 && guard++ < 256) {
            float dir[3];
            for (int k = 0; k < 3; k++) dir[k] = entities[cur].origin[k] - pos[k];
            float len = vlen(dir);
            if (len < 0.0001f) {
                if (path_valid(path_prev(cur), move) < 0) {
                    if (!move) path_project(path_next(cur), cur, origin, dist);
                    return -1;
                }
                cur = path_prev(cur);
            } else if (len > dist) {
                for (int k = 0; k < 3; k++) origin[k] = pos[k] + dir[k] * (dist / len);
                return cur;
            } else {
                dist -= len;
                memcpy(pos, entities[cur].origin, 12);
                memcpy(origin, pos, 12);
                if (path_valid(path_prev(cur), move) < 0) return -1;
                cur = path_prev(cur);
            }
        }
        memcpy(origin, pos, 12);
        return cur;
    }
    while (dist > 0 && guard++ < 256) {
        int nx = path_next(cur);
        if (path_valid(nx, move) < 0) {
            if (!move) path_project(path_prev(cur), cur, origin, dist);
            return -1;
        }
        float dir[3];
        for (int k = 0; k < 3; k++) dir[k] = entities[nx].origin[k] - pos[k];
        float len = vlen(dir);
        if (len < 0.0001f && path_valid(path_next(nx), move) < 0) {
            if (dist == orig_dist) return -1;   // up against a dead end
            return cur;
        }
        if (len > dist) {
            for (int k = 0; k < 3; k++) origin[k] = pos[k] + dir[k] * (dist / len);
            return cur;
        }
        dist -= len;
        memcpy(pos, entities[nx].origin, 12);
        cur = nx;
        memcpy(origin, pos, 12);
    }
    memcpy(origin, pos, 12);
    return cur;
}

// CPathTrack::Nearest (2D)
static int path_nearest(int start, const float *origin)
{
    if (start < 0) return -1;
    float dx = origin[0] - entities[start].origin[0], dy = origin[1] - entities[start].origin[1];
    float best = dx * dx + dy * dy;
    int nearest = start;
    int p = path_next(start), guard = 0;
    while (p >= 0 && p != start && guard++ < 1024) {
        dx = origin[0] - entities[p].origin[0];
        dy = origin[1] - entities[p].origin[1];
        float d = dx * dx + dy * dy;
        if (d < best) { best = d; nearest = p; }
        p = path_next(p);
    }
    return nearest;
}

// CPathTrack::Link for every path_track (after all entities have spawned)
static void path_link_all(void)
{
    for (int i = 1; i < num_entities; i++) entities[i].pnext = entities[i].pprev = entities[i].palt = -1;
    for (int i = 1; i < num_entities; i++) {
        entity_t *e = &entities[i];
        if (e->type != ENT_PATH_TRACK) continue;
        char altname[48];
        str_copy(altname, ent_value(e, "altpath"), sizeof(altname));
        if (e->target[0]) {
            int t = ents_find_targetname(e->target, 1);
            if (t >= 0 && entities[t].type == ENT_PATH_TRACK) {
                e->pnext = t;
                // SetPrevious: unless we are the target's alternate path
                char talt[48];
                str_copy(talt, ent_value(&entities[t], "altpath"), sizeof(talt));
                if (strcasecmp(e->targetname, talt)) entities[t].pprev = i;
            }
        }
        if (altname[0]) {
            int t = ents_find_targetname(altname, 1);
            if (t >= 0 && entities[t].type == ENT_PATH_TRACK) {
                e->palt = t;
                char talt[48];
                str_copy(talt, ent_value(&entities[t], "altpath"), sizeof(talt));
                if (strcasecmp(e->targetname, talt)) entities[t].pprev = i;
            }
        }
    }
}

static void path_track_use(entity_t *e)
{
    if (e->palt >= 0) e->spawnflags ^= SF_PATH_ALTERNATE;   // toggle between the two paths
    else e->spawnflags ^= SF_PATH_DISABLED;                // toggle enabled/disabled
}

// ------------------------------------------------------------------ moving brush entities
static void vec_to_angles(const float *d, float *a)
{
    float yaw = 0, pitch = 0;
    if (d[1] == 0 && d[0] == 0) {
        pitch = d[2] > 0 ? 90 : 270;
    } else {
        yaw = atan2f(d[1], d[0]) * 180 / M_PI;
        if (yaw < 0) yaw += 360;
        float f = sqrtf(d[0] * d[0] + d[1] * d[1]);
        pitch = atan2f(d[2], f) * 180 / M_PI;
        if (pitch < 0) pitch += 360;
    }
    a[0] = pitch; a[1] = yaw; a[2] = 0;
}

static float fix_angle(float a) { while (a < 0) a += 360; while (a > 360) a -= 360; return a; }
static float angle_distance(float next, float cur)
{
    float d = next - cur;
    while (d < -180) d += 360;
    while (d > 180) d -= 360;
    return d;
}

// move a brush entity by (delta, dyaw) and carry the player if they stand on it (SV_Push/SV_PushRotate)
static void push_entity(entity_t *e, int idx, const float *delta, float dyaw)
{
    float porg[3];
    pm_get_origin(porg);
    bool riding = e->solid == SOLID_BSP && pm_entity_for_physent(pm_get_groundent()) == idx;
    float oldorg[3];
    memcpy(oldorg, e->origin, 12);
    for (int i = 0; i < 3; i++) e->origin[i] += delta[i];
    e->angles[1] = fmodf(e->angles[1] + dyaw + 360.0f, 360.0f);
    if (!riding && e->model > 0) {
        // nowhere near the player: nothing to push or carry (skip rebuilding the player's collision)
        bmodel_t *bm = &world.bmodels[e->model];
        bool rot = e->angles[0] != 0 || e->angles[1] != 0 || e->angles[2] != 0 || dyaw != 0;
        float r = 0;
        if (rot) for (int j = 0; j < 3; j++) r = fmaxf(r, fmaxf(fabsf(bm->mins[j]), fabsf(bm->maxs[j]))) * 1.74f;
        bool far = false;
        for (int k = 0; k < 3 && !far; k++) {
            float lo = (rot ? e->origin[k] - r : e->origin[k] + bm->mins[k]) - fabsf(delta[k]) - 96;
            float hi = (rot ? e->origin[k] + r : e->origin[k] + bm->maxs[k]) + fabsf(delta[k]) + 96;
            far = porg[k] < lo || porg[k] > hi;
        }
        if (far) return;
    }
    pm_build_physents(); // collision must see the mover at its new position
    if (riding || (e->solid == SOLID_BSP && pm_player_in_entity(idx, porg))) {
        float rel[3] = {porg[0] - oldorg[0], porg[1] - oldorg[1], porg[2] - oldorg[2]};
        float a = dyaw * M_PI / 180.0f, ca = cosf(a), sa = sinf(a);
        float np[3] = {oldorg[0] + delta[0] + rel[0] * ca - rel[1] * sa,
                       oldorg[1] + delta[1] + rel[0] * sa + rel[1] * ca,
                       porg[2] + delta[2] + (delta[2] > 0 ? 0.01f : 0)};
        if (!pm_position_free(np)) {
            // a sloped train pitched under the player: lift them out
            for (int up = 1; up <= 18; up += 1) {
                np[2] += 1;
                if (pm_position_free(np)) break;
            }
        }
        if (pm_position_free(np)) {
            pm_set_origin(np);
            if (riding && dyaw != 0) game_add_view_yaw(dyaw);
        }
    }
}

// ------------------------------------------------------------------ func_tracktrain (plats.cpp CFuncTrackTrain)

static void fire_from(int activator, const char *name)
{
    int old = cur_activator;
    cur_activator = activator;
    char m[48];
    str_copy(m, name, sizeof(m));
    ents_fire_targets(m);
    cur_activator = old;
}

static void tt_find(entity_t *e)
{
    int t = ents_find_targetname(e->target, 1);
    if (t < 0 || entities[t].type != ENT_PATH_TRACK) { e->path = -2; return; }
    e->path = t;
    float pos[3];
    memcpy(pos, entities[t].origin, 12);
    pos[2] += e->height;
    float look[3];
    memcpy(look, pos, 12);
    look[2] -= e->height;
    path_look_ahead(t, look, e->length, 0);
    look[2] += e->height;
    float d[3] = {look[0] - pos[0], look[1] - pos[1], look[2] - pos[2]};
    vec_to_angles(d, e->angles);
    e->angles[1] += 180;   // the train actually points west
    if (e->spawnflags & SF_TRACKTRAIN_NOPITCH) e->angles[0] = 0;
    memcpy(e->origin, pos, 12);
    e->curspeed = ent_float(e, "startspeed", 0);
}

static void tt_nearest_path(entity_t *e)
{
    int best = -1;
    float bestd = 1024;
    for (int i = 1; i < num_entities; i++) {
        if (entities[i].type != ENT_PATH_TRACK) continue;
        float d[3] = {e->origin[0] - entities[i].origin[0], e->origin[1] - entities[i].origin[1], e->origin[2] - entities[i].origin[2]};
        float l = vlen(d);
        if (l < bestd) { bestd = l; best = i; }
    }
    if (best < 0) { e->path = -2; e->curspeed = 0; return; }
    int nx = path_next(best);
    if (nx >= 0) {
        float a[3] = {e->origin[0] - entities[nx].origin[0], e->origin[1] - entities[nx].origin[1], e->origin[2] - entities[nx].origin[2]};
        if (vlen(a) < bestd) best = nx;
    }
    e->path = best;
}

static void tt_dead_end(entity_t *e, int idx)
{
    int p = e->path;
    if (p >= 0) {
        int guard = 0, nx;
        do {
            nx = path_valid(e->oldspeed < 0 ? path_prev(p) : path_next(p), 1);
            if (nx >= 0) p = nx;
        } while (nx >= 0 && guard++ < 256);
        const char *nn = ent_value(&entities[p], "netname");
        if (nn) fire_from(idx, nn);
    }
}

// CFuncTrackTrain::Use (USE_TOGGLE / USE_ON)
static void tt_use(entity_t *e, int idx)
{
    if (e->curspeed == 0) e->curspeed = e->speed;   // m_speed * m_dir
    else e->curspeed = 0;
    e->deadend = false;
}

// USE_SET from the driver: four speed steps forwards and backwards
static void tt_use_set(entity_t *e, float value)
{
    float delta = ((int)(e->curspeed * 4) / (int)(e->speed > 0 ? e->speed : 1)) * 0.25f + 0.25f * value;
    if (delta > 1) delta = 1;
    if (delta < -1) delta = -1;
    if ((e->spawnflags & SF_TRACKTRAIN_FORWARDONLY) && delta < 0) delta = 0;
    e->curspeed = e->speed * delta;
    e->deadend = false;
}

// CFuncTrackTrain::Next, run every frame (the PC train thinks with FL_ALWAYSTHINK)
// CFuncTrackTrain::UpdateSound / StopSound (+ the client's EV_TrainPitchAdjust)
static void tt_sound(entity_t *e, int idx)
{
    static const char *noise[] = {NULL, "plats/ttrain1.wav", "plats/ttrain2.wav", "plats/ttrain3.wav",
                                  "plats/ttrain4.wav", "plats/ttrain6.wav", "plats/ttrain7.wav"};
    int s = (int)ent_float(e, "sounds", 0);
    if (s <= 0 || s > 6) return;
    float vol = ent_float(e, "volume", 0) * 0.1f;
    if (vol == 0) vol = 1.0f;
    float speed = e->deadend ? e->oldspeed : e->curspeed;
    bool playing = e->snd > 0 && snd_slot_playing(e->snd - 1);
    if (speed != 0) {
        int pitch = 60 + (int)(fabsf(speed) * (200 - 60) / 1000);
        if (!playing) {
            snd_emit(idx, CHAN_ITEM, "plats/ttrain_start1.wav", vol, ATTN_NORM, PITCH_NORM, NULL);
            e->snd = snd_emit(idx, CHAN_STATIC, noise[s], vol, ATTN_NORM, pitch, NULL) + 1;
        } else {
            snd_adjust(e->snd - 1, vol, pitch);
        }
    } else if (e->snd > 0) {
        snd_stop(idx, CHAN_STATIC, noise[s]);
        snd_emit(idx, CHAN_ITEM, "plats/ttrain_brake1.wav", vol, ATTN_NORM, PITCH_NORM, NULL);
        e->snd = 0;
    }
}

static void tracktrain_think(entity_t *e, int idx, float dt)
{
    if (e->path >= 0) tt_sound(e, idx);
    if (e->path == -1) {
        if (g_time >= 0.1f) tt_find(e);
        return;
    }
    if (e->path == PATH_NEAREST) { tt_nearest_path(e); return; }
    if (e->path < 0) return;          // no path, or carried by a func_trackchange
    if (e->deadend) {
        // rolling into the dead end
        float d[3] = {e->dest[0] - e->origin[0], e->dest[1] - e->origin[1], e->dest[2] - e->origin[2]};
        float l = vlen(d), step = fabsf(e->oldspeed) * dt;
        float mv[3];
        bool arrived = l <= step || l < 0.01f;
        for (int k = 0; k < 3; k++) mv[k] = arrived ? d[k] : d[k] / l * step;
        push_entity(e, idx, mv, 0);
        if (arrived) { e->deadend = false; tt_dead_end(e, idx); }
        return;
    }
    if (e->curspeed == 0) return;

    float next[3];
    memcpy(next, e->origin, 12);
    next[2] -= e->height;
    int pnext = path_look_ahead(e->path, next, e->curspeed * 0.1f, 1);
    next[2] += e->height;
    float vel[3];
    for (int k = 0; k < 3; k++) vel[k] = (next[k] - e->origin[k]) * 10;

    float front[3];
    memcpy(front, e->origin, 12);
    front[2] -= e->height;
    path_look_ahead(e->path, front, e->length > 0 ? e->length : 100, 0);
    front[2] += e->height;
    float d[3] = {front[0] - e->origin[0], front[1] - e->origin[1], front[2] - e->origin[2]};
    float ang[3];
    vec_to_angles(d, ang);
    ang[1] += 180;
    ang[0] = fix_angle(ang[0]); ang[1] = fix_angle(ang[1]);
    e->angles[0] = fix_angle(e->angles[0]); e->angles[1] = fix_angle(e->angles[1]);
    if (pnext < 0 || (d[0] == 0 && d[1] == 0)) memcpy(ang, e->angles, 12);
    float vx = (e->spawnflags & SF_TRACKTRAIN_NOPITCH) ? 0 : angle_distance(ang[0], e->angles[0]);
    float vy = angle_distance(ang[1], e->angles[1]);

    if (pnext >= 0) {
        if (pnext != e->path) {
            int pfire = e->curspeed >= 0 ? pnext : e->path;
            e->path = pnext;
            entity_t *pf = &entities[pfire];
            const char *msg = ent_value(pf, "message");
            if (msg && !(pf->spawnflags & SF_PATH_FIRED)) {
                if (pf->spawnflags & SF_PATH_FIREONCE) pf->spawnflags |= SF_PATH_FIRED;
                fire_from(idx, msg);
            }
            if (pf->spawnflags & SF_PATH_DISABLE_TRAIN) e->spawnflags |= SF_TRACKTRAIN_NOCONTROL;
            if ((e->spawnflags & SF_TRACKTRAIN_NOCONTROL) && pf->speed != 0) e->curspeed = pf->speed;
        }
        float t = dt > 0.1f ? 0.1f : dt;
        float mv[3] = {vel[0] * t, vel[1] * t, vel[2] * t};
        float dyaw = vy * 10 * t, dpitch = vx * 10 * t;
        push_entity(e, idx, mv, dyaw);
        e->angles[0] = fix_angle(e->angles[0] + dpitch);
    } else {
        // end of the path: roll to the dead end, then stop (CFuncTrackTrain::DeadEnd)
        e->oldspeed = e->curspeed;
        e->curspeed = 0;
        memcpy(e->dest, next, 12);
        float rest[3] = {next[0] - e->origin[0], next[1] - e->origin[1], next[2] - e->origin[2]};
        if (vlen(rest) > 0.01f) e->deadend = true;
        else tt_dead_end(e, idx);
    }
}

// CFuncTrackTrain::OnControls
static bool tt_on_controls(entity_t *t, const float *porg)
{
    if (t->spawnflags & SF_TRACKTRAIN_NOCONTROL) return false;
    float off[3] = {porg[0] - t->origin[0], porg[1] - t->origin[1], porg[2] - t->origin[2]};
    float f[3], r[3], u[3];
    angle_vectors(t->angles, f, r, u);
    float local[3] = {off[0] * f[0] + off[1] * f[1] + off[2] * f[2], -(off[0] * r[0] + off[1] * r[1] + off[2] * r[2]),
                      off[0] * u[0] + off[1] * u[1] + off[2] * u[2]};
    for (int k = 0; k < 3; k++)
        if (local[k] < t->ctlmins[k] || local[k] > t->ctlmaxs[k]) return false;
    return true;
}

static void stop_driving(void)
{
    driving = -1;
    driving_global[0] = 0;
    pm_set_ontrain(false);
}

bool ents_player_driving(void) { return driving >= 0; }
void ents_player_teleported(void) { stop_driving(); }

// CBasePlayer::PlayerUse: +use on a train's controls starts or stops driving it
bool ents_player_train_use(void)
{
    if (driving >= 0) { stop_driving(); return true; }
    int ge = pm_entity_for_physent(pm_get_groundent());
    if (ge <= 0 || ge >= num_entities) return false;
    entity_t *t = &entities[ge];
    if (t->type != ENT_TRACKTRAIN) return false;
    float porg[3];
    pm_get_origin(porg);
    if (!tt_on_controls(t, porg)) return false;
    driving = ge;
    str_copy(driving_global, ent_value(t, "globalname"), sizeof(driving_global));
    pm_set_ontrain(true);
    snd_play("plats/train_use1.wav", 0.8f, 4);
    return true;
}

// CBasePlayer::PreThink train speed control; returns the HUD speed indicator frame
int ents_player_train_update(int buttons, int pressed)
{
    if (driving < 0) return -1;
    entity_t *t = &entities[driving];
    int ge = pm_entity_for_physent(pm_get_groundent());
    // jumping off, strafing, or the train's controls going dead turn driving off
    if (ge != driving || (t->spawnflags & SF_TRACKTRAIN_NOCONTROL) || (buttons & ((1 << 9) | (1 << 10)))) {
        stop_driving();
        return -1;
    }
    float zero[3] = {0, 0, 0};
    pm_set_velocity(zero);
    if (pressed & (1 << 3)) tt_use_set(t, 1);        // IN_FORWARD
    else if (pressed & (1 << 4)) tt_use_set(t, -1);  // IN_BACK
    // TrainSpeed(): back, stop, slow, medium, fast -> frames of the 320hud3 train sprite
    float f = t->curspeed / (t->speed > 0 ? t->speed : 1);
    if (t->curspeed < 0) return 0;
    if (t->curspeed == 0) return 1;
    if (f < 0.33f) return 2;
    if (f < 0.66f) return 3;
    return 4;
}

// ------------------------------------------------------------------ func_trackchange / func_trackautochange
static bool is_trackauto(entity_t *e) { return !strcasecmp(e->classname, "func_trackautochange"); }

static void trackchange_update_targets(entity_t *e, int state)
{
    if (e->tracktop < 0 || e->trackbottom < 0) return;
    if (!is_trackauto(e)) {
        // CFuncTrackChange: only the track at the platform's position is enabled
        if (state == MS_AT_TOP) entities[e->tracktop].spawnflags &= ~SF_PATH_DISABLED;
        else entities[e->tracktop].spawnflags |= SF_PATH_DISABLED;
        if (state == MS_AT_BOTTOM) entities[e->trackbottom].spawnflags &= ~SF_PATH_DISABLED;
        else entities[e->trackbottom].spawnflags |= SF_PATH_DISABLED;
        return;
    }
    // CFuncTrackAuto: enable the track beyond the target position, disable the other
    int target, other;
    if (e->targetstate == MS_AT_TOP) { target = path_next(e->tracktop); other = path_next(e->trackbottom); }
    else { target = path_next(e->trackbottom); other = path_next(e->tracktop); }
    if (target >= 0) {
        entities[target].spawnflags &= ~SF_PATH_DISABLED;
        if (e->code == TRAIN_FOLLOWING && e->train >= 0 && entities[e->train].curspeed == 0)
            tt_use(&entities[e->train], e->train);
    }
    if (other >= 0) entities[other].spawnflags |= SF_PATH_DISABLED;
}

static void trackchange_find(entity_t *e)
{
    char n[48];
    str_copy(n, ent_value(e, "toptrack"), sizeof(n));
    int top = ents_find_targetname(n, 1);
    str_copy(n, ent_value(e, "bottomtrack"), sizeof(n));
    int bottom = ents_find_targetname(n, 1);
    str_copy(n, ent_value(e, "train"), sizeof(n));
    int train = ents_find_targetname(n, 1);
    if (top < 0 || bottom < 0 || train < 0 || entities[train].type != ENT_TRACKTRAIN) return;
    bmodel_t *bm = &world.bmodels[e->model > 0 ? e->model : 0];
    float c[3];
    for (int k = 0; k < 3; k++) c[k] = e->origin[k] + (bm->mins[k] + bm->maxs[k]) * 0.5f;
    e->tracktop = path_nearest(top, c);
    e->trackbottom = path_nearest(bottom, c);
    e->train = train;
    trackchange_update_targets(e, e->state);
}

// CFuncTrackChange::EvaluateTrain
static int trackchange_evaluate(entity_t *e, int pcur)
{
    if (pcur < 0 || e->train < 0) return TRAIN_SAFE;
    entity_t *t = &entities[e->train];
    if (t->path == pcur || (entities[pcur].pprev >= 0 && t->path == entities[pcur].pprev) ||
        (entities[pcur].pnext >= 0 && t->path == entities[pcur].pnext)) {
        if (t->curspeed != 0) return TRAIN_BLOCKING;
        float dx = e->origin[0] - t->origin[0], dy = e->origin[1] - t->origin[1];
        float l = sqrtf(dx * dx + dy * dy);
        if (l < t->length) return TRAIN_FOLLOWING;
        if (l > 150 + t->length) return TRAIN_SAFE;
        return TRAIN_BLOCKING;
    }
    return TRAIN_SAFE;
}

static void trackchange_go(entity_t *e)
{
    bool down = e->state == MS_AT_TOP;
    if (is_trackauto(e)) e->targetstate = down ? MS_AT_BOTTOM : MS_AT_TOP;
    trackchange_update_targets(e, down ? MS_GOING_DOWN : MS_GOING_UP);
    e->state = down ? MS_GOING_DOWN : MS_GOING_UP;
    memcpy(e->dest, down ? e->pos2 : e->pos1, 12);
    const float *adest = down ? e->ang1 : e->ang2;
    float d[3] = {e->dest[0] - e->origin[0], e->dest[1] - e->origin[1], e->dest[2] - e->origin[2]};
    float l = vlen(d);
    float dyaw = angle_distance(adest[1], e->angles[1]);
    float time = (e->spawnflags & SF_TRACK_DONT_MOVE) ? fabsf(dyaw) / e->speed : l / e->speed;
    if (time < 0.1f) time = 0.1f;
    e->avelocity[0] = 0;
    e->avelocity[1] = dyaw / time;
    e->avelocity[2] = 0;
    e->ltime = time;
    e->active = true;
    if (e->code == TRAIN_FOLLOWING) entities[e->train].path = PATH_CARRIED;
    mover_sound_start(e);
}

static void trackchange_use(entity_t *e)
{
    if (e->state != MS_AT_TOP && e->state != MS_AT_BOTTOM) return;
    if (e->train < 0) trackchange_find(e);
    int pcur = e->state == MS_AT_TOP ? e->tracktop : e->trackbottom;
    if (!is_trackauto(e)) {
        e->code = trackchange_evaluate(e, pcur);
        if (e->code == TRAIN_BLOCKING) { snd_emit(e - entities, CHAN_VOICE, "buttons/button11.wav", 1.0f, ATTN_NORM, PITCH_NORM, NULL); return; }
        trackchange_go(e);
        return;
    }
    // CFuncTrackAuto::Use
    if (cur_activator >= 0 && entities[cur_activator].type == ENT_TRACKTRAIN) {
        e->code = trackchange_evaluate(e, pcur);
        if (e->code == TRAIN_FOLLOWING && e->state != e->targetstate) trackchange_go(e);
    } else {
        int t = pcur >= 0 ? path_next(pcur) : -1;
        if (t >= 0 && e->train >= 0 && entities[e->train].path != t)
            e->targetstate = e->targetstate == MS_AT_TOP ? MS_AT_BOTTOM : MS_AT_TOP;
        trackchange_update_targets(e, e->targetstate);
    }
}

static void trackchange_think(entity_t *e, int idx, float dt)
{
    if (e->train == -1 && g_time >= 2.0f) { e->train = -2; trackchange_find(e); }   // CFuncTrackChange::Find
    if (!e->active) return;
    float d[3] = {e->dest[0] - e->origin[0], e->dest[1] - e->origin[1], e->dest[2] - e->origin[2]};
    float l = vlen(d);
    float step = (e->spawnflags & SF_TRACK_DONT_MOVE) ? 0 : e->speed * dt;
    e->ltime -= dt;
    bool arrived = e->ltime <= 0;
    float delta[3];
    for (int k = 0; k < 3; k++) delta[k] = (arrived || l <= step) ? d[k] : d[k] / l * step;
    float dyaw = e->avelocity[1] * dt;
    bool down = e->state == MS_GOING_DOWN;
    if (arrived) dyaw = angle_distance(down ? e->ang1[1] : e->ang2[1], e->angles[1]);
    float pivot[3];
    memcpy(pivot, e->origin, 12);
    push_entity(e, idx, delta, dyaw);
    // carry the train: rotate its offset around the platform and move it along (UpdateTrain)
    if (e->code == TRAIN_FOLLOWING && e->train >= 0) {
        entity_t *t = &entities[e->train];
        float rel[3] = {t->origin[0] - pivot[0], t->origin[1] - pivot[1], t->origin[2] - pivot[2]};
        float a = dyaw * M_PI / 180.0f, ca = cosf(a), sa = sinf(a);
        float nd[3] = {pivot[0] + delta[0] + rel[0] * ca - rel[1] * sa - t->origin[0],
                       pivot[1] + delta[1] + rel[0] * sa + rel[1] * ca - t->origin[1], delta[2]};
        push_entity(t, e->train, nd, dyaw);
    }
    if (arrived) {
        e->active = false;
        e->state = down ? MS_AT_BOTTOM : MS_AT_TOP;
        mover_sound_stop(e);
        if (e->code == TRAIN_FOLLOWING && e->train >= 0) {
            // SetTrack: the train continues from the new track
            entity_t *t = &entities[e->train];
            int trk = down ? e->trackbottom : e->tracktop;
            t->path = path_nearest(trk, t->origin);
            if (t->path < 0) t->path = trk;
        }
        trackchange_update_targets(e, e->state);
    }
}

static float frame_dt_for_use;
void ents_think(float dt)
{
    frame_dt_for_use = dt;
    if (pushable_settle_pending) { settle_pushables(); game_trace_begin_frame(); }
    g_time += dt;
    // delayed events
    in_event_pass = true;
    for (int i = 0; i < num_events;) {
        if (events[i].time <= g_time) {
            char name[48];
            memcpy(name, events[i].name, 48);
            int act = events[i].activator, caller = events[i].caller;
            events[i] = events[--num_events];
            int old = cur_activator, oldc = cur_caller;
            cur_activator = act;
            cur_caller = caller;
            ents_fire_targets(name);
            cur_activator = old;
            cur_caller = oldc;
        } else i++;
    }
    in_event_pass = false;

    float porg[3], pmins[3], pmaxs[3];
    pm_get_origin(porg);
    pm_get_hull(pmins, pmaxs);

#ifdef PROFX
    static uint32_t pt[MAX_ENTITIES]; static int pframes; uint32_t tlast = TICKS_READ(); int ptype = 0;
    static uint32_t ploop; uint32_t tloop0 = tlast;
#define PROFX_STEP(ty) do { uint32_t _n = TICKS_READ(); pt[ptype] += TICKS_DISTANCE(tlast, _n); tlast = _n; ptype = (ty); } while (0)
#else
#define PROFX_STEP(ty)
#endif
    // only the entity types that do something every frame (a map has hundreds of others)
    static uint16_t think_list[MAX_ENTITIES];
    static int num_think, think_built = -1;
    static int think_gen = -1;
    if (think_built != num_entities || think_gen != ents_gen) {
        num_think = 0;
        for (int i = 1; i < num_entities; i++) {
            switch (entities[i].type) {
            case ENT_AMBIENT: case ENT_BUTTON: case ENT_CAMERA: case ENT_CDAUDIO: case ENT_DOOR: case ENT_DOOR_ROTATING:
            case ENT_ENDSECTION: case ENT_ENV_SPARK: case ENT_GLOBAL_BEAM: case ENT_HEALTHCHARGER: case ENT_MONSTERMAKER:
            case ENT_PLAT: case ENT_PUSHABLE: case ENT_RECHARGE: case ENT_ROTATING: case ENT_ROT_BUTTON: case ENT_SENTENCE:
            case ENT_SPEAKER: case ENT_TARGET_CDAUDIO: case ENT_TRACKAUTO: case ENT_TRACKTRAIN: case ENT_TRAIN:
            case ENT_TRIGGER_AUTO: case ENT_TRIGGER_CHANGELEVEL: case ENT_TRIGGER_HURT: case ENT_TRIGGER_MULTIPLE:
            case ENT_TRIGGER_ONCE: case ENT_TRIGGER_PUSH: case ENT_TRIGGER_TELEPORT:
            case ENT_TRIGGER_GRAVITY: case ENT_FRICTION: case ENT_AUTOSAVE: case ENT_MOMENTARY_DOOR:
            case ENT_MOMENTARY_BUTTON: case ENT_PENDULUM: case ENT_TANK:
                think_list[num_think++] = i;
                break;
            default:
                break;
            }
        }
        think_built = num_entities;
        think_gen = ents_gen;
    }
    float friction_touch = 1;   // func_friction under the player this frame
    for (int ti = 0; ti < num_think; ti++) {
        int i = think_list[ti];
        entity_t *e = &entities[i];
        if (e->removed) continue;
        PROFX_STEP(i);
        switch (e->type) {
        case ENT_DOOR:
        case ENT_DOOR_ROTATING:
        case ENT_BUTTON:
        case ENT_ROT_BUTTON:
        case ENT_PLAT:
            if (e->active) mover_step(e, i, dt);
            if (e->type == ENT_PLAT && e->ang2[1] != 0 && e->pos1[2] != e->pos2[2])
                e->angles[1] = e->ang2[1] * (e->origin[2] - e->pos2[2]) / (e->pos1[2] - e->pos2[2]);
            if (e->nextthink > 0 && g_time >= e->nextthink) {
                e->nextthink = 0;
                if (e->type == ENT_BUTTON || e->type == ENT_ROT_BUTTON) {
                    e->state = MS_GOING_DOWN;
                    if (e->type == ENT_ROT_BUTTON) move_angular(e, e->ang1);
                    else move_linear(e, e->pos1);
                } else if (e->type == ENT_PLAT) {
                    e->state = MS_GOING_DOWN;
                    move_linear(e, e->pos2);
                    mover_sound_start(e);
                } else {
                    door_go_down(e);
                }
            }
            // touch-activated doors (no targetname, not use-only)
            if ((e->type == ENT_DOOR || e->type == ENT_DOOR_ROTATING) && !e->targetname[0] && e->contents >= 0 &&
                !(e->spawnflags & 256) && e->state == MS_AT_BOTTOM && master_ok(e)) {
                float mn[3], mx[3];
                for (int k = 0; k < 3; k++) { mn[k] = pmins[k] - 2; mx[k] = pmaxs[k] + 2; }
                if (player_touches(e, porg, mn, mx)) door_go_up(e, true);
            }
            // plats rise when stood on
            if (e->type == ENT_PLAT && e->state == MS_AT_BOTTOM &&
                pm_entity_for_physent(pm_get_groundent()) == i) {
                e->state = MS_GOING_UP;
                move_linear(e, e->pos1);
                mover_sound_start(e);
            }
            break;
        case ENT_TRAIN:
            if (e->path < 0 && e->nextthink > 0 && g_time >= e->nextthink) {
                // CFuncTrain::Activate: teleport to the first corner
                e->nextthink = 0;
                int t = ents_find_targetname(e->target, 1);
                if (t >= 0) {
                    float c[3];
                    model_center(e, c);
                    for (int k = 0; k < 3; k++) e->origin[k] = entities[t].origin[k] - c[k];
                    e->path = t;
                    str_copy(e->target, entities[t].target, sizeof(e->target));
                    if (!e->targetname[0]) train_next(e);
                    else { e->triggered = true; e->state = MS_AT_TOP; }
                }
            } else if (e->active) {
                float old[3];
                memcpy(old, e->origin, 12);
                float d[3], l2 = 0;
                for (int k = 0; k < 3; k++) { d[k] = e->dest[k] - e->origin[k]; l2 += d[k] * d[k]; }
                float l = sqrtf(l2), step = e->speed * dt, delta[3];
                bool arrived = l <= step;
                for (int k = 0; k < 3; k++) delta[k] = arrived ? d[k] : d[k] / l * step;
                memcpy(e->origin, old, 12);
                push_entity(e, i, delta, 0);
                if (arrived) { e->active = false; train_wait(e); }
            } else if (e->nextthink > 0 && g_time >= e->nextthink) {
                e->nextthink = 0;
                if (e->state == MS_GOING_UP) train_wait(e); else train_next(e);
            }
            break;
        case ENT_TRIGGER_AUTO:
            // triggers.cpp CAutoTrigger: fire once shortly after the map starts
            if (!e->removed && g_time > 0.2f) {
                if (global_ok(e)) queue_fire(e->target, e->delay);   // CAutoTrigger: only with its global state on
                e->removed = true;
            }
            break;
        case ENT_TRACKAUTO:
            trackchange_think(e, i, dt);
            break;
        case ENT_TRACKTRAIN:
            tracktrain_think(e, i, dt);
            break;
        case ENT_SENTENCE:
        case ENT_CAMERA:
            if (e->type == ENT_SENTENCE && !e->triggered && e->ltime > 0 && g_time >= e->ltime) {
                e->triggered = true;   // CScriptedSentence::DelayThink
                e->ltime = 0;
                if (!e->targetname[0]) e->nextthink = g_time + 0.1f;
            }
            if (e->nextthink > 0 && g_time >= e->nextthink) {
                e->nextthink = 0;
                if (e->type == ENT_SENTENCE) sentence_find_think(e);
                else game_set_camera(-1, 0, false);   // trigger_camera's wait is over
            }
            break;
        case ENT_AMBIENT:
            if (e->nextthink > 0 && g_time >= e->nextthink) {   // start-up of a looping ambient
                e->nextthink = 0;
                if (e->active) ambient_play(e);
            }
            break;
        case ENT_SPEAKER:
#ifndef NOSPEAKER
            if (e->nextthink > 0 && g_time >= e->nextthink) { e->nextthink = 0; speaker_think(e); }
#endif
            break;
        case ENT_CDAUDIO:
            if (e->model > 0 && player_touches(e, porg, pmins, pmaxs)) { play_cd_track((int)e->health); e->removed = true; }
            break;
        case ENT_TARGET_CDAUDIO: {
            // CTargetCDAudio::Think: checks every half second for the player within its radius
            float r = ent_float(e, "radius", 128);
            float d2 = 0;
            for (int k = 0; k < 3; k++) d2 += (porg[k] - e->origin[k]) * (porg[k] - e->origin[k]);
            if (d2 <= r * r) { play_cd_track((int)e->health); e->removed = true; }
            break;
        }
        case ENT_PUSHABLE:
            if (e->model > 0 && !(e->spawnflags & 0x80)) push_pushable(e, porg, pmins, pmaxs, dt);
            break;
        case ENT_GLOBAL_BEAM:
            // env_beam / env_laser (effects.cpp): drawn while on (StartOn = 1, or toggled by use)
            if (e->active && g_time >= e->ltime2) {
                e->ltime2 = g_time + 0.1f;
                beam_cache_t *bc = &beam_cache[i];
                if (!bc->init) {   // resolve the endpoints and colour once (string lookups are slow)
                    char a[48], b[48];
                    str_copy(a, ent_value(e, "LightningStart"), sizeof(a));
                    bc->laser = !strcasecmp(e->classname, "env_laser");
                    str_copy(b, ent_value(e, bc->laser ? "LaserTarget" : "LightningEnd"), sizeof(b));
                    bc->ia = a[0] ? ents_find_targetname(a, 1) : -1;
                    bc->ib = b[0] ? ents_find_targetname(b, 1) : -1;
                    parse_color(e, "rendercolor", bc->c, 3);
                    if (!bc->c[0] && !bc->c[1] && !bc->c[2]) { bc->c[0] = 255; bc->c[1] = 255; bc->c[2] = 255; }
                    bc->radius = ent_float(e, "Radius", 0);
                    bc->strike = ent_float(e, "StrikeTime", 0);
                    const char *tx = ent_value(e, "texture");
                    bc->plain = tx && strstr(tx, "laserbeam");
                    bc->init = true;
                }
                if (bc->ib < 0 && bc->radius > 0) {
                    // CLightning::RandomArea: an arc from the start to a random wall within "Radius"
                    // (the G-Man's portal at the end, Xen crystal zaps)
                    float pa[3];
                    memcpy(pa, e->origin, 12);
                    if (bc->ia >= 0) ents_sound_origin(bc->ia, pa);
                    float dd = 0;
                    for (int k = 0; k < 3; k++) dd += (pa[k] - porg[k]) * (pa[k] - porg[k]);
                    if (dd > 2000.0f * 2000.0f) break;
                    if (bc->strike < 0) e->ltime2 = g_time + 0.05f + (rand() % 100) / 100.0f * -bc->strike;
                    else if (bc->strike > 0) e->ltime2 = g_time + fminf(bc->strike, 0.25f);
                    for (int tries = 0; tries < 6; tries++) {
                        float d[3] = {(rand() % 200 - 100) / 100.0f, (rand() % 200 - 100) / 100.0f, (rand() % 200 - 100) / 100.0f};
                        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                        if (l < 0.1f) continue;
                        float end[3];
                        for (int k = 0; k < 3; k++) end[k] = pa[k] + d[k] / l * bc->radius;
                        gtrace_t tr;
                        game_trace(pa, end, NULL, NULL, TR_IGNORE_MONSTERS | TR_IGNORE_PLAYER, -1, &tr);
                        if (tr.fraction >= 1 || tr.fraction < 0.1f) continue;
                        if (bc->plain) { extern void game_effect_beam_plain(const float *, const float *, int, int, int); game_effect_beam_plain(pa, tr.endpos, bc->c[0], bc->c[1], bc->c[2]); }
                        else game_effect_lightning(pa, tr.endpos, bc->c[0], bc->c[1], bc->c[2]);
                        break;
                    }
                    break;
                }
                if (bc->ib < 0) break;
                float pa[3], pb[3];
                memcpy(pa, e->origin, 12);
                if (bc->ia >= 0) ents_sound_origin(bc->ia, pa);
                ents_sound_origin(bc->ib, pb);
                // nobody sees a beam on the other side of the map: skip making the effect
                float da = 0, db = 0;
                for (int k = 0; k < 3; k++) { da += (pa[k] - porg[k]) * (pa[k] - porg[k]); db += (pb[k] - porg[k]) * (pb[k] - porg[k]); }
                if (da > 2500.0f * 2500.0f && db > 2500.0f * 2500.0f) break;
                if (bc->laser) { extern void game_effect_beam_plain(const float *, const float *, int, int, int); game_effect_beam_plain(pa, pb, bc->c[0], bc->c[1], bc->c[2]); }
                else game_effect_lightning(pa, pb, bc->c[0], bc->c[1], bc->c[2]);
            }
            break;
        case ENT_TRIGGER_GRAVITY:
            // CTriggerGravity::GravityTouch: the player keeps this gravity until another trigger changes it
            if (e->model > 0 && player_touches(e, porg, pmins, pmaxs)) {
                extern void pm_set_gravity(float g);
                float g = ent_float(e, "gravity", 1);
                extern float pm_get_gravity(void);
                if (pm_get_gravity() != (g > 0 ? g : 1)) debugf("gravity %.2f\n", g > 0 ? g : 1);
                pm_set_gravity(g > 0 ? g : 1);
            }
            break;
        case ENT_FRICTION:
            if (e->model > 0 && player_touches(e, porg, pmins, pmaxs)) friction_touch = ent_float(e, "modifier", 100) / 100.0f;
            break;
        case ENT_AUTOSAVE:
            if (e->model > 0 && player_touches(e, porg, pmins, pmaxs)) {
                extern void game_request_autosave(void);
                game_request_autosave();
                e->removed = true;
            }
            break;
        case ENT_MOMENTARY_DOOR: {
            float d[3], l2 = 0;
            for (int k = 0; k < 3; k++) { d[k] = e->dest[k] - e->origin[k]; l2 += d[k] * d[k]; }
            if (l2 > 0.01f) {
                float l = sqrtf(l2), step = e->speed * dt, delta[3];
                for (int k = 0; k < 3; k++) delta[k] = l <= step ? d[k] : d[k] / l * step;
                if (!e->snd) { e->snd = 1; if (e->movesnd) snd_emit(i, CHAN_STATIC, plat_move_sound(e->movesnd), 1, ATTN_NORM, PITCH_NORM, NULL); }
                push_entity(e, i, delta, 0);
            } else if (e->snd) {
                e->snd = 0;
                snd_stop(i, CHAN_STATIC, NULL);
            }
            break;
        }
        case ENT_MOMENTARY_BUTTON: {
            // let go: turn back at "returnspeed" (CMomentaryRotButton::Return)
            float rs = ent_float(e, "returnspeed", 0);
            if (g_time - e->mlast > 0.15f && rs > 0 && e->mpos > 0) momentary_set(e, e->mpos - rs * dt / (e->dist > 0 ? e->dist : 90), false);
            break;
        }
        case ENT_TANK:
            tank_think(e, i, dt, porg);
            break;
        case ENT_PENDULUM:
            if (e->active) {
                // CPendulum::Swing: back and forth through "distance" degrees
                e->mpos += dt * e->speed / (e->dist > 0 ? e->dist : 90);
                float a = sinf(e->mpos) * e->dist * 0.5f;
                for (int k = 0; k < 3; k++) e->angles[k] = e->ang1[k] + e->movedir[k] * a;
            }
            break;
        case ENT_MONSTERMAKER:
            if (e->active && g_time >= e->nextthink) {
                e->nextthink = g_time + e->delay;
                maker_make(e);
            }
            break;
        case ENT_ENV_SPARK:
            if (e->active && e->nextthink > 0 && g_time >= e->nextthink) {
                float dd = 0;
                for (int k = 0; k < 3; k++) dd += (e->origin[k] - porg[k]) * (e->origin[k] - porg[k]);
                if (dd < 1500.0f * 1500.0f) spark_sound(e);   // out of earshot and sight: just keep the timer
                e->nextthink = g_time + 0.1f + (rand() % 100) / 100.0f * ent_float(e, "MaxDelay", 0);
            }
            break;
        case ENT_RECHARGE:
        case ENT_HEALTHCHARGER:
            // CWallHealth::Off / CRecharge::Off once the player lets go of +use
            if (e->charge_on && g_time > e->ltime2 + 0.25f) {
                snd_stop(i, CHAN_STATIC, NULL);
                e->charge_on = 0;
            }
            break;
        case ENT_TRIGGER_HURT:
            if (e->model > 0 && !(e->spawnflags & 8 /* SF_TRIGGER_HURT_NO_CLIENTS */) && e->active &&
                g_time >= e->nextthink && player_touches(e, porg, pmins, pmaxs)) {
                // CTriggerHurt::HurtTouch: dmg is per second, applied every half second
                float dmg = ent_float(e, "dmg", 10) * 0.5f;
                int type = (int)ent_float(e, "damagetype", 0);
                if (dmg < 0) player_take_health(-dmg, type);
                else if (dmg > 0) player_take_damage(dmg, type, NULL, i);
                e->nextthink = g_time + 0.5f;
                if (e->target[0] && !(e->spawnflags & 16 && false)) {
                    fire_entity_target(e);
                    if (e->spawnflags & 1) e->target[0] = 0;   // SF_TRIGGER_HURT_TARGETONCE
                }
            }
            break;
        case ENT_ENDSECTION:
            if (e->model > 0 && !e->targetname[0] && player_touches(e, porg, pmins, pmaxs)) game_end_section();
            break;
        case ENT_ROTATING:
            if (e->active)
                for (int k = 0; k < 3; k++) {
                    e->angles[k] += e->avelocity[k] * e->speed * dt;
                    e->angles[k] = fmodf(e->angles[k], 360.0f);
                }
            break;
        case ENT_TRIGGER_ONCE:
        case ENT_TRIGGER_MULTIPLE:
            if (e->model > 0 && !(e->spawnflags & 2 /* SF_TRIGGER_NOCLIENTS */) && g_time >= e->nextthink &&
                player_touches(e, porg, pmins, pmaxs) && master_ok(e)) {
                fire_entity_target(e);
                if (e->type == ENT_TRIGGER_ONCE) e->removed = true;
                else e->nextthink = g_time + e->wait;
            } else if (num_pushables > 0 && e->model > 0 && (e->spawnflags & 4 /* SF_TRIGGER_PUSHABLES */) && g_time >= e->nextthink && master_ok(e)) {
                // the specimen cart rolling into the spectrometer beam starts the resonance cascade
                for (int pi = 0; pi < num_pushables && pi < MAX_PUSHABLES; pi++) {
                    entity_t *p = &entities[pushable_list[pi]];
                    if (p->type != ENT_PUSHABLE || p->removed || p->model <= 0) continue;
                    if (!player_touches(e, p->origin, world.bmodels[p->model].mins, world.bmodels[p->model].maxs)) continue;
                    fire_entity_target(e);
                    if (e->type == ENT_TRIGGER_ONCE) e->removed = true;
                    else e->nextthink = g_time + e->wait;
                    break;
                }
            }
            break;
        case ENT_TRIGGER_CHANGELEVEL:
            // armed only once the player has stepped out of it (we arrive inside the
            // return trigger after a level transition)
            if (e->model > 0 && !(e->spawnflags & 2)) {
                if (player_touches(e, porg, pmins, pmaxs)) {
                    if (!e->triggered) {
                        e->triggered = true;
                        request_changelevel(e);
                    }
                } else {
                    e->triggered = false;
                }
            }
            break;
        case ENT_TRIGGER_TELEPORT:
            // triggers.cpp CTriggerTeleport::TeleportTouch
            if (e->model > 0 && !(e->spawnflags & 2) && player_touches(e, porg, pmins, pmaxs) && master_ok(e)) {
                int d = ents_find_targetname(e->target, 1);
                if (d >= 0) {
                    float o[3];
                    memcpy(o, entities[d].origin, 12);
                    o[2] += -pmins[2] + 1;      // tmp.z -= pevToucher->mins.z; tmp.z++
                    pm_set_origin(o);
                    float zero[3] = {0, 0, 0};
                    pm_set_velocity(zero);
                    game_set_view_angles(entities[d].angles);
                    ents_player_teleported();
                    memcpy(porg, o, 12);
                }
            }
            break;
        case ENT_TRIGGER_PUSH:
            if (e->active && e->model > 0 && player_touches(e, porg, pmins, pmaxs)) {
                float v[3];
                for (int k = 0; k < 3; k++) v[k] = e->movedir[k] * e->speed;
                pm_add_basevelocity(v);
            }
            break;
        default:
            break;
        }
    }
    { extern void pm_set_friction(float f); pm_set_friction(friction_touch); }
#ifdef PROFX
    PROFX_STEP(0);
    ploop += TICKS_DISTANCE(tloop0, TICKS_READ());
    if (++pframes >= 30) {
        debugf("  ents prof loop %d us (us/frame):", (int)TICKS_TO_US(ploop / pframes)); ploop = 0;
        for (int k = 1; k < num_entities; k++) if (pt[k] / pframes > TICKS_FROM_US(40)) debugf(" %s[%s]=%d", entities[k].classname, entities[k].targetname, (int)TICKS_TO_US(pt[k] / pframes));
        debugf("\n");
        memset(pt, 0, sizeof(pt)); pframes = 0;
    }
#endif
}

// h_battery.cpp CRecharge::Use / healthkit.cpp CWallHealth::Use (called every frame +use is held)
static void charger_use(entity_t *e, int idx)
{
    bool health = e->type == ENT_HEALTHCHARGER;
    if (e->juice <= 0 || !player.suit) {
        if (g_time >= e->sndtime) {
            e->sndtime = g_time + 0.62f;
            snd_emit(idx, CHAN_ITEM, health ? "items/medshotno1.wav" : "items/suitchargeno1.wav", 1.0f, ATTN_NORM, PITCH_NORM, NULL);
        }
        if (e->charge_on) { snd_stop(idx, CHAN_STATIC, NULL); e->charge_on = 0; }
        return;
    }
    e->ltime2 = g_time;
    if (g_time < e->ltime) return;
    if (!e->charge_on) {
        e->charge_on = 1;
        snd_emit(idx, CHAN_ITEM, health ? "items/medshot4.wav" : "items/suitchargeok1.wav", 1.0f, ATTN_NORM, PITCH_NORM, NULL);
        e->sndtime = g_time + 0.56f;
    }
    if (e->charge_on == 1 && g_time >= e->sndtime) {
        e->charge_on = 2;
        snd_emit(idx, CHAN_STATIC, health ? "items/medcharge4.wav" : "items/suitcharge1.wav", health ? 1.0f : 0.85f, ATTN_NORM, PITCH_NORM, NULL);
    }
    if (health) {
        if (player_take_health(1, DMG_GENERIC)) e->juice--;
    } else if (player.armor < 100) {
        e->juice--;
        player.armor += 1;
        if (player.armor > 100) player.armor = 100;
    }
    e->ltime = g_time + 0.1f;
}

void ents_use_entity(int ent)
{
    if (ent > 0 && ent < num_entities) ent_use(&entities[ent]);
}

// CBasePlayer::PlayerUse: the best usable object in front of the player (brushes and monsters).
// Chargers keep working while +use is held (FCAP_CONTINUOUS_USE); everything else on the press.
bool ents_player_use_hold(const float *eye, const float *forward, bool pressed)
{
    int best = -1;
    float bestdot = 0.7f; // VIEW_FIELD_NARROW
    for (int i = 1; i < num_entities; i++) {
        entity_t *e = &entities[i];
        if (e->removed || e->model <= 0) continue;
        bool usable = e->type == ENT_BUTTON || e->type == ENT_ROT_BUTTON || e->type == ENT_RECHARGE || e->type == ENT_MOMENTARY_BUTTON ||
                      e->type == ENT_TANKCONTROLS ||
                      e->type == ENT_HEALTHCHARGER ||
                      ((e->type == ENT_DOOR || e->type == ENT_DOOR_ROTATING) && !e->targetname[0]) ||
                      (e->type == ENT_PLAT && !e->targetname[0]);
        if (!usable) continue;
        bmodel_t *bm = &world.bmodels[e->model];
        float c[3], d2 = 0;
        for (int k = 0; k < 3; k++) {
            float mn = e->origin[k] + bm->mins[k], mx = e->origin[k] + bm->maxs[k];
            float p = eye[k] < mn ? mn : (eye[k] > mx ? mx : eye[k]);
            d2 += (p - eye[k]) * (p - eye[k]);
            c[k] = (mn + mx) * 0.5f - eye[k];
        }
        if (d2 > 64 * 64) continue;
        float l = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        if (l < 0.001f) l = 0.001f;
        float dot = (c[0] * forward[0] + c[1] * forward[1] + c[2] * forward[2]) / l;
        if (d2 < 16 * 16) dot += 0.3f; // right next to it
        if (dot > bestdot) { bestdot = dot; best = i; }
    }
    int npc = npc_find_usable(eye, forward, &bestdot);
    if (npc >= 0) {
        if (pressed) npc_player_use(npc);
        return true;
    }
    if (best >= 0) {
        entity_t *e = &entities[best];
        if (e->type == ENT_RECHARGE || e->type == ENT_HEALTHCHARGER) { charger_use(e, best); return true; }
        if (e->type == ENT_TANKCONTROLS) { if (pressed) tank_controls_use(e); return true; }
        if (e->type == ENT_MOMENTARY_BUTTON) {
            // held: keep turning; a new press turns it the other way (always forward with the door hack)
            bool first = pressed && e->mpos <= 0.001f;
            if (pressed) e->mdir = (e->spawnflags & 1) ? 1 : (e->mpos >= 0.999f ? -1 : (e->mpos <= 0.001f ? 1 : -e->mdir));
            float dps = e->speed / (e->dist > 0 ? e->dist : 90);
            momentary_set(e, e->mpos + e->mdir * dps * frame_dt_for_use, first);
            e->mlast = g_time;
            return true;
        }
        if (!pressed) return true;
        if (e->type == ENT_DOOR || e->type == ENT_DOOR_ROTATING) {
            if (!master_ok(e)) { door_locked_sound(e); return true; }
            if (e->state == MS_AT_BOTTOM || e->state == MS_GOING_DOWN) door_go_up(e, true);
            else if (e->wait == -1 || (e->spawnflags & 32)) door_go_down(e);
        } else if ((e->type == ENT_BUTTON || e->type == ENT_ROT_BUTTON) && !master_ok(e)) {
            door_locked_sound(e);
        } else {
            ent_use(e);
        }
        return true;
    }
    if (pressed) snd_emit(0, CHAN_ITEM, "common/wpn_denyselect.wav", 0.4f, ATTN_NORM, PITCH_NORM, NULL);
    return false;
}

// where an entity's sounds come from: brush models from their centre, monsters from their body
bool ents_sound_origin(int ent, float *out)
{
    if (ent <= 0 || ent >= num_entities) return false;
    entity_t *e = &entities[ent];
    if (npc_origin_for_entity(ent, out)) { out[2] += 36; return true; }
    if (e->model > 0) {
        bmodel_t *bm = &world.bmodels[e->model];
        for (int k = 0; k < 3; k++) out[k] = e->origin[k] + (bm->mins[k] + bm->maxs[k]) * 0.5f;
        return true;
    }
    memcpy(out, e->origin, 12);
    return true;
}

// ------------------------------------------------------------------ globalname transfer across level changes
// The engine carries entities with a "globalname" (e.g. the intro tram) into the next map.
typedef struct { char name[48]; float rel[3]; float angles[3]; float curspeed, speed; char path[48]; int state; bool used; } gstate_t;
static gstate_t gstates[16];
static int num_gstates;

void ents_save_globals(const float *landmark)
{
    num_gstates = 0;
    driving = -1;   // re-attached by name in ents_restore_globals
    for (int i = 1; i < num_entities && num_gstates < 16; i++) {
        entity_t *e = &entities[i];
        const char *g = ent_value(e, "globalname");
        if (!g || e->removed) continue;
        gstate_t *gs = &gstates[num_gstates++];
        str_copy(gs->name, g, sizeof(gs->name));
        for (int k = 0; k < 3; k++) gs->rel[k] = e->origin[k] - landmark[k];
        memcpy(gs->angles, e->angles, 12);
        gs->curspeed = e->curspeed;
        gs->speed = e->speed;
        gs->state = e->state;
        gs->path[0] = 0;
        if (e->path >= 0) str_copy(gs->path, entities[e->path].targetname, sizeof(gs->path));
        gs->used = false;
    }
}

static void queue_fire(const char *name, float delay);
void ents_fire_delayed(const char *name, float delay) { queue_fire(name, delay > 0 ? delay : 0.1f); }
void ents_restore_globals(const float *landmark)
{
    if (carry_target[0]) { queue_fire(carry_target, carry_delay > 0 ? carry_delay : 0.1f); carry_target[0] = 0; }
    for (int i = 1; i < num_entities; i++) {
        entity_t *e = &entities[i];
        const char *g = ent_value(e, "globalname");
        if (!g) continue;
        for (int k = 0; k < num_gstates; k++) {
            gstate_t *gs = &gstates[k];
            if (gs->used || strcasecmp(gs->name, g)) continue;
            gs->used = true;
            for (int j = 0; j < 3; j++) e->origin[j] = gs->rel[j] + landmark[j];
            memcpy(e->angles, gs->angles, 12);
            e->curspeed = gs->curspeed;
            e->speed = gs->speed;
            if (e->type == ENT_TRAIN) {
                e->path = gs->path[0] ? ents_find_targetname(gs->path, 1) : -1;
                e->nextthink = 0;
            }
            if (e->type == ENT_TRACKTRAIN) {
                e->path = PATH_NEAREST;       // CFuncTrackTrain::OverrideReset -> NearestPath
                e->deadend = false;
                if (driving_global[0] && !strcasecmp(driving_global, g)) driving = i;
            }
            break;
        }
    }
    num_gstates = 0;
}

// ------------------------------------------------------------------ save-game snapshot of the live level
// removed / active / triggered bits for every map entity, plus position and motion of brush entities
typedef struct { uint16_t idx; uint8_t state, fl; float o[3], a[3], curspeed, think; int16_t path, pad; } esnap_t;
int ents_snapshot(uint8_t *b, int max, int base)
{
    if (max < 384 + 2) return 0;
    memset(b, 0, 384);
    for (int i = 1; i < base && i < MAX_ENTITIES; i++) {
        entity_t *e = &entities[i];
        if (e->removed) b[i >> 3] |= 1 << (i & 7);
        if (e->active) b[128 + (i >> 3)] |= 1 << (i & 7);
        if (e->triggered) b[256 + (i >> 3)] |= 1 << (i & 7);
    }
    int n = 0, pos = 386;
    for (int i = 1; i < base; i++) {
        entity_t *e = &entities[i];
        if (e->removed || e->model <= 0 || pos + (int)sizeof(esnap_t) > max) continue;
        esnap_t r = {.idx = i, .state = e->state, .fl = (e->visible ? 1 : 0) | (e->solid != SOLID_NOT ? 2 : 0),
                     .curspeed = e->curspeed, .think = e->nextthink > 0 ? e->nextthink - g_time : -1, .path = e->path};
        memcpy(r.o, e->origin, 12); memcpy(r.a, e->angles, 12);
        memcpy(b + pos, &r, sizeof r); pos += sizeof r; n++;
    }
    b[384] = n & 255; b[385] = n >> 8;
    return pos;
}
void ents_apply_snapshot(const uint8_t *b, int len, int base)
{
    if (len < 386) return;
    for (int i = 1; i < base && i < MAX_ENTITIES; i++) {
        entity_t *e = &entities[i];
        if (b[i >> 3] & (1 << (i & 7))) { e->removed = true; e->visible = false; e->solid = SOLID_NOT; }
        e->active = (b[128 + (i >> 3)] >> (i & 7)) & 1;
        e->triggered = (b[256 + (i >> 3)] >> (i & 7)) & 1;
    }
    int n = b[384] | (b[385] << 8), pos = 386;
    for (int k = 0; k < n && pos + (int)sizeof(esnap_t) <= len; k++, pos += sizeof(esnap_t)) {
        esnap_t r; memcpy(&r, b + pos, sizeof r);
        if (r.idx >= base) continue;
        entity_t *e = &entities[r.idx];
        memcpy(e->origin, r.o, 12); memcpy(e->angles, r.a, 12);
        e->state = r.state; e->visible = r.fl & 1; e->solid = (r.fl & 2) ? SOLID_BSP : SOLID_NOT;
        e->curspeed = r.curspeed; e->path = r.path;
        e->nextthink = r.think >= 0 ? g_time + r.think : 0;
    }
}
