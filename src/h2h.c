// HL64 - Head-To-Head: the PlayStation 2 version's two-player split-screen deathmatch.
//
// The two players share the world (doors, items, projectiles, decals) but each has their own
// copy of everything the single-player code keeps about "the player": the variables marked PCTX
// (the pctx linker section: health, weapons, view, HUD state ...) and the player's part of Valve's
// playermove_t. h2h_switch() swaps those between the players, so movement, weapons, the HUD and
// the suit run unchanged for each in turn. Each player sees the other as a "puppet" character
// (npc.c, NF_PUPPET) that bullets, explosions and projectiles hit like any monster; the damage is
// handed over to that player at the start of their next turn.
//
// The rules are CHalfLifeMultiplay (dlls/multiplay_gamerules.cpp): frag and time limits, a frag
// per kill and minus one for dying by your own hand or the world, respawning at an
// info_player_deathmatch with a crowbar, a pistol and 68 rounds, items coming back after 20-30
// seconds, the active weapon dropped where a player dies, and the deathmatch weapon values
// (skill.c). The PS2 front end (guidata/frontend.pak guidef.txt) offers six characters, a frag
// limit of 1-50 and a time limit of 0-30 minutes.
#include <libdragon.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "game.h"
#include "npc.h"
#include "studio.h"
#include "weapons.h"
#include "font.h"
#include "h2h.h"

// each player's gun model: the weapon it is for, the model (freed with the map: reset per match)
static const char *gun_last[2]; static void *gun_got[2]; static int gun_tries[2];


bool h2h_active;
int h2h_cur;
int h2h_inflictor = -1;

extern char __start_pctx[], __stop_pctx[];
int  pm_ctx_size(void);
void pm_ctx_save(void *buf);
void pm_ctx_load(const void *buf);
void pm_reset_player(const float *origin);
void pm_get_view_ofs(float *v);
int  pm_get_groundent(void);
void game_set_view_angles(const float *angles);
float game_view_yaw(void);
void weapons_player_reset(void);
const char *weapons_class(int id);
float player_dead_time(void);

const char *const h2h_char_names[H2H_NUM_CHARS] = {"Gordon Freeman", "Gina Cross", "Colette Green", "Security Guard", "Soldier", "Scientist"};
static const char *const char_models[H2H_NUM_CHARS] = {"pl_gordon", "pl_gina", "pl_colette", "pl_barney", "pl_soldier", "pl_sci"};
const char *const h2h_maps[H2H_NUM_MAPS] = {"basement", "debris", "water_canal", "skirmish", "waypoint", "signal", "office", "snark_pit2", "datacore2", "stalkyard2", "crimson"};
const char *const h2h_map_titles[H2H_NUM_MAPS] = {"Basement", "Debris", "Water Canal", "Skirmish", "Waypoint", "Signal", "Office", "Snark Pit", "Datacore", "Stalkyard", "Crimson"};
const char *const h2h_map_credits[H2H_NUM_MAPS] = {[10] = "Map by UnhealthyGrapejuice"};

typedef struct {
    int frags, deaths;
    int character, puppet;
    float eye[3], ang[3];            // the view it was last given (rendering)
    // what the other player sees of this one
    float origin[3], yaw, pitch, speed;
    bool ducking, onground, dead, gibbed;
    int weapon;
    float fire_time;
    // damage the other player did since this one's last turn
    float dmg; int dmg_type, dmg_attacker; float dmg_from[3]; bool dmg_has_from;
    float push[3];
    bool died_counted;
} h2h_pl_t;

static h2h_pl_t pl[2];
static uint8_t *ctx_store[2], *pm_store[2];
static int ctx_size, pmsize;
static int frag_limit = 10, time_limit;
static float match_time, intermission_at;
static int winner = -1;
static char feed[3][48];
static float feed_time[3];

int h2h_frags(int p) { return pl[p].frags; }
int h2h_frag_limit(void) { return frag_limit; }
bool h2h_intermission(void) { return intermission_at > 0; }

// ------------------------------------------------------------------ contexts
// the other player's copy of a PCTX variable (NULL when no match is on)
const void *h2h_other_copy(const void *var)
{
    if (!h2h_active || !ctx_store[0]) return NULL;
    const char *v = var;
    if (v < __start_pctx || v >= __stop_pctx) return NULL;
    return ctx_store[1 - h2h_cur] + (v - __start_pctx);
}

void h2h_switch(int p)
{
    if (p == h2h_cur) return;
    memcpy(ctx_store[h2h_cur], __start_pctx, ctx_size);
    pm_ctx_save(pm_store[h2h_cur]);
    memcpy(__start_pctx, ctx_store[p], ctx_size);
    pm_ctx_load(pm_store[p]);
    h2h_cur = p;
    // the other player's body is there to see and to hit, this one's own is not
    for (int k = 0; k < 2; k++) {
        int n = pl[k].puppet;
        if (n < 0) continue;
        bool own = k == p;
        entities[npcs[n].ent].removed = own || pl[k].gibbed;
        npcs[n].solid = !own && !pl[k].dead;
    }
}

// ------------------------------------------------------------------ spawning
static bool spot_clear(const float *o, int self)
{
    // IsSpawnPointValid: nobody within 128 units; and the hull fits
    for (int k = 0; k < 2; k++) {
        if (k == self || pl[k].dead) continue;
        float d = 0;
        for (int a = 0; a < 3; a++) d += (pl[k].origin[a] - o[a]) * (pl[k].origin[a] - o[a]);
        if (d < 128 * 128) return false;
    }
    return true;
}

static int pick_spawn(int self, float *o, float *ang)
{
    int spots[64], n = 0;
    for (int i = ents_find_class("info_player_deathmatch", 0); i >= 0 && n < 64; i = ents_find_class("info_player_deathmatch", i + 1)) spots[n++] = i;
    if (!n) for (int i = ents_find_class("info_player_start", 0); i >= 0 && n < 64; i = ents_find_class("info_player_start", i + 1)) spots[n++] = i;
    if (!n) return -1;
    // EntSelectSpawnPoint: a random valid spot (the PC walks the list from a random start)
    int start = rand() % n, best = -1;
    for (int k = 0; k < n; k++) {
        int i = spots[(start + k) % n];
        if (spot_clear(entities[i].origin, self)) { best = i; break; }
    }
    if (best < 0) best = spots[start];
    memcpy(o, entities[best].origin, 12);
    o[2] += 1;
    memcpy(ang, entities[best].angles, 12);
    ang[0] = ang[2] = 0;
    return best;
}

static void respawn(int p)
{
    h2h_pl_t *P = &pl[p];
    float o[3], a[3];
    int sp = pick_spawn(p, o, a);
    if (sp < 0) { o[0] = o[1] = o[2] = 0; a[0] = a[1] = a[2] = 0; }
    debugf("h2h: player %d spawns at %.0f %.0f %.0f (spot %d)\n", p + 1, o[0], o[1], o[2], sp);
    pm_reset_player(o);
    game_set_view_angles(a);
    // CBasePlayer::Spawn + CHalfLifeMultiplay::PlayerSpawn
    memset(&player, 0, sizeof(player));
    player.health = 100;
    player.flashbattery = 100;
    player.suit = true;
    player.weapons = 1u << WEAPON_SUIT;
    weapons_player_reset();
    weapons_give("weapon_crowbar");
    weapons_give("weapon_9mmhandgun");
    weapons_give_ammo("ammo_9mmclip"); weapons_give_ammo("ammo_9mmclip");
    weapons_give_ammo("ammo_9mmclip"); weapons_give_ammo("ammo_9mmclip");   // 68 rounds
    memcpy(P->origin, o, 12);
    P->dead = P->gibbed = false;
    P->died_counted = false;
    P->dmg = 0;
    P->push[0] = P->push[1] = P->push[2] = 0;
    // a telefrag would need the other one standing on the spot: pick_spawn keeps 128 units away
    snd_emit(0, CHAN_ITEM, "items/gunpickup2.wav", 0.6f, ATTN_NORM, PITCH_NORM, NULL);
}

void h2h_begin(int char1, int char2, int frags, int minutes)
{
    if (!ctx_store[0]) {
        ctx_size = __stop_pctx - __start_pctx;
        pmsize = pm_ctx_size();
        for (int k = 0; k < 2; k++) { ctx_store[k] = malloc(ctx_size); pm_store[k] = malloc(pmsize); }
        debugf("h2h: player context %d + %d bytes\n", ctx_size, pmsize);
    }
    memset(pl, 0, sizeof(pl));
    memset(gun_last, 0, sizeof(gun_last)); memset(gun_got, 0, sizeof(gun_got));
    pl[0].character = char1; pl[1].character = char2;
    frag_limit = frags; time_limit = minutes;
    match_time = 0; intermission_at = 0; winner = -1;
    memset(feed_time, 0, sizeof(feed_time));
    h2h_active = true;
    h2h_cur = 0;
    for (int k = 0; k < 2; k++) {
        memcpy(ctx_store[k], __start_pctx, ctx_size);
        pm_ctx_save(pm_store[k]);
        pl[k].puppet = npc_spawn_puppet(char_models[pl[k].character], k);
    }
    for (int k = 0; k < 2; k++) {
        h2h_switch(k);
        pl[k].dead = true;     // (not in the way of the first one's spawn check)
        respawn(k);
    }
    h2h_switch(0);
}

void h2h_end(void)
{
    if (!h2h_active) return;
    h2h_switch(0);
    h2h_active = false;
    for (int k = 0; k < 2; k++) if (pl[k].puppet >= 0) entities[npcs[pl[k].puppet].ent].removed = true;
}

// ------------------------------------------------------------------ damage and deaths
void h2h_puppet_hit(int p, float dmg, int dmgtype, const float *dir)
{
    if (p < 0 || p > 1 || pl[p].dead) return;
    h2h_pl_t *P = &pl[p];
    P->dmg += dmg;
    P->dmg_type |= dmgtype;
    P->dmg_attacker = h2h_inflictor;
    // where it came from: the attacker (for the pain indicator)
    if (h2h_inflictor >= 0 && h2h_inflictor != p) { memcpy(P->dmg_from, pl[h2h_inflictor].origin, 12); P->dmg_has_from = true; }
    else if (dir) { for (int k = 0; k < 3; k++) P->dmg_from[k] = P->origin[k] - dir[k] * 64; P->dmg_has_from = true; }
    // knockback (CBaseMonster::TakeDamage on a player: the push away from the inflictor)
    if (dir && !(dmgtype & DMG_FALL)) {
        float force = fminf(dmg * 5, 1000);
        P->push[0] += dir[0] * force; P->push[1] += dir[1] * force; P->push[2] += dir[2] * force;
    }
}

static void feed_add(const char *s)
{
    for (int k = 2; k > 0; k--) { memcpy(feed[k], feed[k - 1], 48); feed_time[k] = feed_time[k - 1]; }
    strncpy(feed[0], s, 47); feed[0][47] = 0;
    feed_time[0] = 4;
}

// called by player_take_damage when the player in the current context dies
void h2h_player_died(bool gibbed)
{
    int v = h2h_cur;
    h2h_pl_t *V = &pl[v];
    if (V->died_counted) return;
    V->died_counted = true;
    V->dead = true;
    V->deaths++;
    int a = h2h_inflictor;
    char msg[64];
    const char *vn = v == 0 ? "Player 1" : "Player 2";
    if (a == v || a < 0) {   // killed self, or by the world
        V->frags--;
        snprintf(msg, sizeof(msg), a == v ? "%s killed self" : "%s died", vn);
    } else {
        pl[a].frags++;
        snprintf(msg, sizeof(msg), "%s killed %s", a == 0 ? "Player 1" : "Player 2", vn);
    }
    feed_add(msg);
    // the active weapon falls where the player died (GR_PLR_DROP_GUN_ACTIVE)
    int w = weapons_current();
    if (w > WEAPON_GLOCK && w != WEAPON_CROWBAR) {
        const char *cls = weapons_class(w);
        if (cls) {
            float o[3]; pm_get_origin(o);
            o[2] -= 20;
            int it = npc_spawn_item(cls, o, rand() % 360);
            if (it >= 0) npcs[it].spawned = true;   // not respawning
        }
    }
    V->gibbed = gibbed;
    debugf("h2h: %s (frags %d / %d)\n", msg, pl[0].frags, pl[1].frags);
}

// ------------------------------------------------------------------ per turn
// before player p's movement: damage and pushes the other player gave it
void h2h_turn_begin(int p)
{
    h2h_pl_t *P = &pl[p];
    if (P->dmg > 0 && !player.dead) {
        float d = P->dmg; P->dmg = 0;
        int t = P->dmg_type; P->dmg_type = 0;
        h2h_inflictor = P->dmg_attacker;
        player_take_damage(d, t, P->dmg_has_from ? P->dmg_from : NULL, -1);
        P->dmg_has_from = false;
    }
    if (P->push[0] || P->push[1] || P->push[2]) {
        float v[3]; pm_get_velocity(v);
        for (int k = 0; k < 3; k++) v[k] += P->push[k];
        if (!player.dead) pm_set_velocity(v);
        P->push[0] = P->push[1] = P->push[2] = 0;
    }
    h2h_inflictor = -1;
}

// a dead player comes back on a button press after a second, or after five (mp_forcerespawn)
bool h2h_turn_dead(int p, bool any_button)
{
    if (!player.dead || intermission_at > 0) return false;
    float t = player_dead_time();
    if ((t > 1 && any_button) || t > 5) { respawn(p); return true; }
    return false;
}

static void pose(int p, float dt)
{
    h2h_pl_t *P = &pl[p];
    int n = P->puppet;
    if (n < 0) return;
    npc_t *N = &npcs[n];
    studio_t *m = N->model;
    if (!m) return;
    memcpy(N->origin, P->origin, 12);
    N->yaw = P->yaw;
    // CBasePlayer::SetAnimation: the weapon's aim / shoot, the crouch and run gaits, deaths
    static const char *const ext[] = {"", "crowbar", "onehanded", "python", "mp5", "", "bow", "shotgun", "rpg", "gauss", "egon",
                                      "hive", "crowbar", "trip", "trip", "squeak"};
    const char *x = P->weapon > 0 && P->weapon < (int)(sizeof(ext) / sizeof(ext[0])) && ext[P->weapon][0] ? ext[P->weapon] : "onehanded";
    char name[40];
    bool once = false;
    if (P->dead) {
        static const char *const deaths[] = {"die_simple", "die_backwards", "die_forwards", "headshot", "gutshot"};
        snprintf(name, sizeof(name), "%s", deaths[(p * 3 + P->deaths) % 5]);
        once = true;
    } else if (!P->onground) {
        snprintf(name, sizeof(name), "jump");
    } else if (P->ducking) {
        if (P->speed > 5) snprintf(name, sizeof(name), "crawl");
        else snprintf(name, sizeof(name), "crouch_%s_%s", P->fire_time > 0 ? "shoot" : "aim", x);
    } else if (P->speed > 5) {
        snprintf(name, sizeof(name), P->speed > 160 ? "run" : "walk");
    } else {
        snprintf(name, sizeof(name), "ref_%s_%s", P->fire_time > 0 ? "shoot" : "aim", x);
    }
    int s = studio_find_seq(m, name);
    if (s < 0) s = studio_find_seq(m, "idle");
    if (s < 0) s = 0;
    if (s != N->seq) { N->seq = s; N->frame = 0; N->seq_done = false; }
    {   // the gun in its hand (the PC's weaponmodel, p_*.mdl); none once dead (it's dropped)
        static const char *const pmodel[] = {NULL, "p_crowbar", "p_9mmhandgun", "p_357", "p_9mmar", NULL, "p_crossbow", "p_shotgun",
                                             "p_rpg", "p_gauss", "p_egon", "p_hgun", "p_grenade", "p_tripmine", "p_satchel", "p_squeak"};
        const char *wn = !P->dead && P->weapon > 0 && P->weapon < (int)(sizeof(pmodel) / sizeof(pmodel[0])) ? pmodel[P->weapon] : NULL;
        if (wn != gun_last[p] || (wn && !gun_got[p] && ++gun_tries[p] % 30 == 0)) {   // (load on a change; retry now and then)
            if (wn != gun_last[p]) gun_tries[p] = 0;
            gun_last[p] = wn;
            gun_got[p] = wn ? studio_get(wn) : NULL;
        }
        N->weapmodel = gun_got[p];
    }
    sseq_t *sq = &m->seqs[s];
    float fps = sq->fps > 0 ? sq->fps : 10;
    float rate = 1;
    if (!once && P->speed > 5 && P->onground) {   // the gait keeps up with the feet (StudioEstimateGait)
        float lm = sqrtf(sq->linmove[0] * sq->linmove[0] + sq->linmove[1] * sq->linmove[1]);
        if (lm > 1 && sq->numframes > 1) rate = P->speed / (lm * fps / (sq->numframes - 1));
        rate = fminf(fmaxf(rate, 0.5f), 2.0f);
    }
    N->frame += dt * fps * rate;
    if (sq->numframes > 1) {
        if (once) {
            if (N->frame >= sq->numframes - 1) { N->frame = sq->numframes - 1; N->seq_done = true; }
        } else N->frame = fmodf(N->frame, sq->numframes - 1);
    }
}

// after player p's turn: remember where it is and what it looks like
void h2h_turn_end(int p, const float *eye, const float *ang, float dt, bool firing)
{
    h2h_pl_t *P = &pl[p];
    memcpy(P->eye, eye, 12);
    memcpy(P->ang, ang, 12);
    pm_get_origin(P->origin);
    float v[3]; pm_get_velocity(v);
    P->speed = sqrtf(v[0] * v[0] + v[1] * v[1]);
    P->yaw = game_view_yaw();
    P->ducking = pm_ducking();
    P->onground = pm_get_groundent() >= 0;
    P->dead = player.dead;
    P->weapon = weapons_current();
    if (firing) P->fire_time = 0.25f;
    else if (P->fire_time > 0) P->fire_time -= dt;
    pose(p, dt);
}

void h2h_view(int p, float *eye, float *ang) { memcpy(eye, pl[p].eye, 12); memcpy(ang, pl[p].ang, 12); }
void h2h_origin(int p, float *o) { memcpy(o, pl[p].origin, 12); }
#ifdef H2HDBG
void h2h_debug(void)
{
    for (int k = 0; k < 2; k++) {
        npc_t *n = &npcs[pl[k].puppet];
        debugf("H2H p%d at %.0f %.0f %.0f yaw %.0f hp %s frags %d deaths %d dead %d seq %d %s frame %.1f\n", k, pl[k].origin[0], pl[k].origin[1], pl[k].origin[2], pl[k].yaw,
               "", pl[k].frags, pl[k].deaths, pl[k].dead, n->seq, n->model ? n->model->seqs[n->seq].name : "?", n->frame);
    }
}
#endif

// ------------------------------------------------------------------ rules
// CHalfLifeMultiplay::Think: the frag and time limits; true when the match is over and the
// intermission has run its course
bool h2h_think(float dt, bool any_button)
{
    for (int k = 0; k < 3; k++) if (feed_time[k] > 0) feed_time[k] -= dt;
    if (intermission_at > 0) {
        intermission_at += dt;
        return intermission_at > 6 && (any_button || intermission_at > 30);
    }
    match_time += dt;
    bool over = time_limit > 0 && match_time >= time_limit * 60;
    for (int k = 0; k < 2; k++) if (frag_limit > 0 && pl[k].frags >= frag_limit) over = true;
    if (over) {
        intermission_at = 0.001f;
        winner = pl[0].frags > pl[1].frags ? 0 : (pl[1].frags > pl[0].frags ? 1 : -1);
        snd_emit(0, CHAN_STATIC, "items/suitchargeok1.wav", 1, ATTN_NONE, 100, NULL);
    }
    return false;
}

// ------------------------------------------------------------------ HUD (in the current player's half)
void h2h_draw_hud(int p)
{
    font_begin(BLEND_ALPHA);
    char b[48];
    int y = hud_y0 + 4;
    font_color(RGBA32(255, 160, 0, 255));
    snprintf(b, sizeof(b), "Frags %d", pl[p].frags);
    font_draw(FONT_SMALL, 12 + 4, y + (hud_y0 ? 2 : 8), b);
    if (time_limit > 0 && intermission_at <= 0) {
        int left = (int)(time_limit * 60 - match_time);
        if (left < 0) left = 0;
        snprintf(b, sizeof(b), "%d:%02d", left / 60, left % 60);
        font_draw_centered(FONT_SMALL, 160, y, b);
    }
    font_color(RGBA32(255, 210, 120, 255));
    int fy = hud_y0 + hud_h / 2 - 40;
    for (int k = 0; k < 3; k++) {
        if (feed_time[k] <= 0) continue;
        font_draw_centered(FONT_SMALL, 160, fy - k * 11, feed[k]);
    }
    if (player.dead && intermission_at <= 0 && player_dead_time() > 1) {
        font_color(RGBA32(255, 160, 0, 255));
        font_draw_centered(FONT_SMALL, 160, hud_y0 + hud_h - 34, "Press a button to respawn");
    }
    if (intermission_at > 0) {
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_set_prim_color(RGBA32(0, 0, 0, 150));
        rdpq_fill_rectangle(40, hud_y0 + 18, 280, hud_y0 + hud_h - 18);
        font_begin(BLEND_ALPHA);
        font_color(RGBA32(255, 160, 0, 255));
        const char *t = winner < 0 ? "Draw" : (winner == p ? "You win!" : "You lose");
        font_draw_centered(FONT_MEDIUM, 160, hud_y0 + 26, t);
        font_color(RGBA32(196, 181, 80, 255));
        font_draw(FONT_SMALL, 208, hud_y0 + 42, "Frags Deaths");
        font_color(RGBA32(255, 160, 0, 255));
        for (int k = 0; k < 2; k++) {
            snprintf(b, sizeof(b), "Player %d  (%s)", k + 1, h2h_char_names[pl[k].character]);
            font_draw(FONT_SMALL, 48, hud_y0 + 54 + k * 14, b);
            snprintf(b, sizeof(b), "%3d     %3d", pl[k].frags, pl[k].deaths);
            font_draw(FONT_SMALL, 208, hud_y0 + 54 + k * 14, b);
        }
        if (intermission_at > 6) font_draw_centered(FONT_SMALL, 160, hud_y0 + hud_h - 32, "Press a button");
    }
}
