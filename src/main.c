// HL64 - Half-Life for the Nintendo 64: main loop, input, view, and level loading.
#include <libdragon.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include "bsp.h"
#include "game.h"
#include "render.h"
#include "studio.h"
#include "npc.h"
#include "font.h"
#include "hud.h"
#include "menu.h"
#include "weapons.h"
#include "nav.h"
#ifdef TESTCFG
#include "testcfg.h"
#endif

// IN_* button bits from Valve's common/in_buttons.h
#define IN_ATTACK   (1 << 0)
#define IN_JUMP     (1 << 1)
#define IN_DUCK     (1 << 2)
#define IN_FORWARD  (1 << 3)
#define IN_BACK     (1 << 4)
#define IN_USE      (1 << 5)
#define IN_MOVELEFT (1 << 9)
#define IN_MOVERIGHT (1 << 10)
#define IN_RUN      (1 << 12)

void pm_get_origin(float *o);
void pm_set_origin(const float *o);
void pm_get_velocity(float *v);
void pm_set_velocity(const float *v);
void pm_get_view_ofs(float *v);
void pm_get_punch(float *v);
int  pm_get_groundent(void);
void pm_reset_player(const float *origin);
void pm_clear_basevelocity(void);
void pm_set_ontrain(bool on);

// ------------------------------------------------------------------ state
int trace_frames;
static uint32_t prof_lights, prof_gtb, prof_npc, prof_ents, prof_pm, prof_play, prof_rend;
static player_input_t input;
static float view_angles[3];
static bool noclip;
static bool in_game;
static float game_clock;
static float fps_value;
static int prev_buttons;
static float turn_held;

static char maplist[160][16];
static int num_maps;

static char pending_map[32];
static char pending_landmark[48];
static bool pending_change;
static bool end_requested;

// maps whose chapter title has been shown this game (CWorld::Spawn shows it on arrival)
static char visited[160][16];
static int num_visited;
static float title_delay;
static char title_name[48];

// trigger_camera
static int cam_ent = -1;
static bool cam_freeze, cam_snap;
static float cam_angles[3];

static float last_eye[3], last_ang[3];

// view shake (UTIL_ScreenShake / V_ApplyShake)
static float shake_amp, shake_freq, shake_end, shake_dur;
void game_shake(float amplitude, float frequency, float duration)
{
    if (amplitude > 16) amplitude = 16;
    if (game_clock + duration > shake_end || amplitude > shake_amp) {
        shake_amp = amplitude;
        shake_freq = frequency;
        shake_dur = duration;
        shake_end = game_clock + duration;
    }
}
float game_view_yaw(void) { return view_angles[1]; }

// where the player arrived in this map (a death restarts from here)
static char cur_map[32];
static float arrive_origin[3], arrive_angles[3];
static bool weapon_fire, weapon_fire2, weapon_reload;
static int weapon_select;

void game_add_view_yaw(float dyaw) { view_angles[1] = fmodf(view_angles[1] + dyaw + 360.0f, 360.0f); }

void game_set_view_angles(const float *a)
{
    view_angles[0] = a[0];
    view_angles[1] = a[1];
    view_angles[2] = 0;
}

void hud_message(const char *msg) { hud_show_message(msg); }

void game_set_camera(int ent, float wait, bool freeze)
{
    (void)wait;
    cam_ent = ent;
    cam_freeze = ent >= 0 && freeze;
    cam_snap = true;
}

void game_end_section(void) { end_requested = true; }

void game_request_changelevel(const char *map, const char *landmark)
{
    if (!map || !map[0] || pending_change) return;
    strncpy(pending_map, map, sizeof(pending_map) - 1);
    pending_map[sizeof(pending_map) - 1] = 0;
    strncpy(pending_landmark, landmark ? landmark : "", sizeof(pending_landmark) - 1);
    pending_change = true;
}

// Debug menu (menu.c)
bool dbg_god, dbg_noclip, dbg_give_all;
static bool dbg_noclip_prev;
int game_num_maps(void);
const char *game_map_name(int i);
static void load_maplist(void)
{
    FILE *f = fopen("rom:/maps/maplist.txt", "r");
    num_maps = 0;
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f) && num_maps < 160) {
        char *p = line;
        while (*p && *p != '\n' && *p != '\r' && *p != ' ') p++;
        *p = 0;
        if (line[0]) { strncpy(maplist[num_maps], line, 15); num_maps++; }
    }
    fclose(f);
}

int game_num_maps(void) { return num_maps; }
const char *game_map_name(int i) { return i >= 0 && i < num_maps ? maplist[i] : ""; }
static bool map_in_rom(const char *name)
{
    for (int i = 0; i < num_maps; i++)
        if (!strcasecmp(maplist[i], name)) return true;
    return false;
}

static void setup_sky(void)
{
    const char *sn = ent_value(&entities[0], "skyname");
    char skyname[32];
    strncpy(skyname, sn ? sn : "desert", sizeof(skyname) - 1);
    skyname[sizeof(skyname) - 1] = 0;
    for (char *c = skyname; *c; c++) *c = tolower((unsigned char)*c);
    render_load_sky(skyname);
    sky_color = (color_t){0x48, 0x50, 0x60, 0xff};
    int le = ents_find_class("light_environment", 0);
    if (le >= 0) {
        const char *v = ent_value(&entities[le], "_light");
        int r = 128, g = 128, b = 128;
        if (v) sscanf(v, "%d %d %d", &r, &g, &b);
        float m = fmaxf(r, fmaxf(g, b));
        if (m < 1) m = 1;
        sky_color = (color_t){(uint8_t)(r / m * 150), (uint8_t)(g / m * 160), (uint8_t)(b / m * 190), 0xff};
    }
}

static bool flag_key(const char *key)
{
    const char *v = ent_value(&entities[0], key);
    return v && atoi(v) != 0;
}

// spawn: fresh start at info_player_start, or via landmark transition
void npcs_mark_base(void);
static bool autosave_pending;
static void autosave(void);
// Debug map loader: enter a map where the previous one leads into it (maps/arrive.txt from
// tools/build_assets.py), with the transition's changetarget (c1a2: the elevator ride)
typedef struct { char landmark[48], target[48]; float ofs[3], delay; } arrive_t;
static arrive_t *debug_arrive;
static bool find_arrive(const char *map, arrive_t *a)
{
    FILE *f = fopen("rom:/maps/arrive.txt", "r");
    if (!f) return false;
    char line[160], m[32];
    bool ok = false;
    while (!ok && fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%31s %47s %f %f %f %47s %f", m, a->landmark, &a->ofs[0], &a->ofs[1], &a->ofs[2], a->target, &a->delay) == 7 && !strcasecmp(m, map)) ok = true;
    }
    fclose(f);
    if (ok && !strcmp(a->target, "-")) a->target[0] = 0;
    return ok;
}

static bool load_map(const char *name, const char *landmark, const float *old_landmark)
{
    float porigin[3], pvel[3];
    pm_get_origin(porigin);
    pm_get_velocity(pvel);
    extern bool pm_ducking(void);
    bool pduck = pm_ducking();
    snd_stop_all();
    menu_draw_loading(0.1f);
    rspq_wait(); // the RSP/RDP may still be reading the previous map's vertices and textures
    { extern void menu_release_title(void); menu_release_title(); }
    render_free_map(); // free the old map's render data first so the big map buffer fits
    studio_free_all();
    nav_load(NULL);
    num_npcs = 0;
    bsp_free();
#ifdef MAPCYCLE
    { heap_stats_t hs; sys_get_heap_stats(&hs); int lo = 0, hi = 8 << 20; while (hi - lo > 4096) { int mid = (lo + hi) / 2; void *volatile p = malloc(mid); if (p) { free(p); lo = mid; } else hi = mid; }
      debugf("heap after free: %d KB used, largest block %d KB\n", hs.used / 1024, lo / 1024); }
#ifdef HEAPDBG
    { extern void heapdbg_dump(void); heapdbg_dump();
      // where are the free regions? take the biggest blocks one after another
      void *held[6]; int nh = 0;
      for (; nh < 6; nh++) { int lo2 = 0, hi2 = 8 << 20; while (hi2 - lo2 > 4096) { int mid = (lo2 + hi2) / 2; void *volatile q = malloc(mid); if (q) { free(q); lo2 = mid; } else hi2 = mid; }
          if (lo2 < 65536) break;
          held[nh] = malloc(lo2); debugf("FREEBLOCK %p %d KB\n", held[nh], lo2 / 1024); }
      if (nh >= 2) { uint8_t *g = (uint8_t *)held[0] + 0; (void)g;
          // print the bytes between the two biggest blocks
          uint8_t *lo_end = (uint8_t *)held[0], *hi_start = (uint8_t *)held[1];
          if (hi_start < lo_end) { uint8_t *t = lo_end; lo_end = hi_start; hi_start = t; }
          (void)hi_start;
          debugf("GAP dump after first block:\n");
          uint32_t *w = (uint32_t *)((((uintptr_t)held[0] + 3356 * 1024) & ~7) );
          for (int r = 0; r < 24; r++) debugf("%p: %08lx %08lx %08lx %08lx\n", w + r * 4, w[r*4], w[r*4+1], w[r*4+2], w[r*4+3]);
      }
      while (nh > 0) free(held[--nh]); }
#endif
#endif
    if (!bsp_load(name)) {
        hud_message("Failed to load map");
        return false;
    }
    menu_draw_loading(0.5f);
    nav_load(name);
    ents_spawn_all();
    npcs_spawn_all();
    npcs_mark_base();
    { extern void lights_map_start(void); lights_map_start(); }
    float origin[3] = {0, 0, 0};
    bool placed = false;
    if (landmark && landmark[0] && old_landmark) {
        for (int i = ents_find_class("info_landmark", 0); i >= 0; i = ents_find_class("info_landmark", i + 1)) {
            if (!strcasecmp(entities[i].targetname, landmark)) {
                float lm[3];
                memcpy(lm, entities[i].origin, 12);
                for (int k = 0; k < 3; k++) origin[k] = porigin[k] - old_landmark[k] + lm[k];
                ents_restore_globals(lm);
                npcs_restore_transition(lm);
                placed = true;
                break;
            }
        }
    }
    npc_scripts_init();
    menu_draw_loading(0.8f);
    render_new_map();
    setup_sky();
    cam_ent = -1;
    if (!placed && debug_arrive) {
        for (int i = ents_find_class("info_landmark", 0); i >= 0; i = ents_find_class("info_landmark", i + 1)) {
            if (strcasecmp(entities[i].targetname, debug_arrive->landmark)) continue;
            hud_reset();
            ents_player_teleported();
            for (int k = 0; k < 3; k++) origin[k] = entities[i].origin[k] + debug_arrive->ofs[k];
            int s = ents_find_class("info_player_start", 0);
            view_angles[0] = view_angles[2] = 0;
            view_angles[1] = s >= 0 ? entities[s].angles[1] : 0;
            extern bool pm_find_free_position_hull(float *origin, int usehull);
            extern void pm_set_ducked(void);
            extern bool pm_position_free(const float *pos);
            extern void pm_build_physents(void);
            pm_reset_player(origin);
            pm_build_physents();
            {   // the trigger can be up near the ceiling (a button-fired elevator): stand on the floor below
                static const float hmn[3] = {-16, -16, -36}, hmx[3] = {16, 16, 36};
                float down[3] = {origin[0], origin[1], origin[2] - 256};
                gtrace_t tr;
                game_trace(origin, down, hmn, hmx, TR_IGNORE_PLAYER | TR_IGNORE_MONSTERS, -1, &tr);
                if (!tr.startsolid) memcpy(origin, tr.endpos, 12);
            }
            float duck[3] = {origin[0], origin[1], origin[2] - 18};
            bool stand = pm_position_free(origin);
            if (!stand) { pm_set_ducked(); stand = !pm_position_free(duck); }   // a crawlspace (t0a0a)
            if (!stand) { memcpy(origin, duck, 12); pm_reset_player(origin); pm_set_ducked(); }
            else { pm_find_free_position(origin); pm_reset_player(origin); }
            if (debug_arrive->target[0]) { extern void ents_fire_delayed(const char *n, float d); ents_fire_delayed(debug_arrive->target, debug_arrive->delay); }
            placed = true;
            break;
        }
    }
    if (!placed) {
        hud_reset();
        ents_player_teleported();
        int s = ents_find_class("info_player_start", 0);
        if (s < 0) s = ents_find_class("info_player_deathmatch", 0);
        if (s >= 0) {
            memcpy(origin, entities[s].origin, 12);
            origin[2] += 1;
            pm_find_free_position(origin);
            view_angles[0] = 0;
            view_angles[1] = entities[s].angles[1];
            view_angles[2] = 0;
        }
        pm_reset_player(origin);
    } else {
        // the landmark offset can land outside the new map's geometry (the player kept walking
        // after the transition was triggered): move to the nearest free spot
        // keep crouching through a transition in a crawlspace (t0a0 -> t0a0a): standing up there
        // used to put the player somewhere else, often back into the return trigger
        extern bool pm_position_free(const float *pos);
        extern bool pm_find_free_position_hull(float *origin, int usehull);
        extern void pm_set_ducked(void);
        float st[3] = {origin[0], origin[1], origin[2]};
        bool stand_ok = false;
        if (!pduck) pm_find_free_position(origin);
        else {
            // ducked origin is the duck hull's centre: standing needs 18 units more above it
            extern void pm_build_physents(void);
            pm_reset_player(st);
            pm_build_physents();
            st[2] += 18;
            stand_ok = pm_position_free(st);
            if (stand_ok) memcpy(origin, st, 12);
            else pm_find_free_position_hull(origin, 1);
        }
        pm_reset_player(origin);
        if (pduck && !stand_ok) pm_set_ducked();
        pm_set_velocity(pvel);
        if (ents_player_driving()) pm_set_ontrain(true);
    }
    // world.cpp CWorld::Spawn / CBasePlayer: start dark, game title, chapter title
    if (flag_key("startdark")) hud_set_dark();
    if (flag_key("gametitle")) hud_show_game_title();
    const char *ct = ent_value(&entities[0], "chaptertitle");
    bool seen = false;
    for (int i = 0; i < num_visited; i++) if (!strcasecmp(visited[i], name)) seen = true;
    if (!seen && num_visited < 160) strncpy(visited[num_visited++], name, 15);
    if (ct && !seen) {
        strncpy(title_name, ct, sizeof(title_name) - 1);
        title_delay = 0.3f;
    }
    strncpy(cur_map, name, sizeof(cur_map) - 1);
    pm_get_origin(arrive_origin);
    memcpy(arrive_angles, view_angles, 12);
    player_map_start();
    weapons_map_start();
    game_trace_begin_frame();
    debugf("map %s start %.0f %.0f %.0f\n", name, origin[0], origin[1], origin[2]);
    autosave_pending = true;
    return true;
}

// ---- saves (EEPROM 16kbit = 2KB): slot 0 = autosave, slots 1..5 = manual saves ----
typedef struct { uint32_t magic, serial; char map[32]; float o[3], a[3]; int skill; player_t pl; } save_t;
#define SAVE_MAGIC 0x484C3636
// SRAM 32KB: 3 slots of 10KB, each the player plus a snapshot of the level (entities, characters)
#define SAVE_SLOT_BYTES 10240
#define NUM_SAVE_SLOTS 3
#define SAVE_HDR 336
_Static_assert(sizeof(save_t) <= SAVE_HDR, "save header too big");
static union { save_t s; uint8_t raw[SAVE_SLOT_BYTES]; } save_buf __attribute__((aligned(16)));
int ents_snapshot(uint8_t *b, int max, int base);
void ents_apply_snapshot(const uint8_t *b, int len, int base);
void npcs_mark_base(void);
int npcs_base_ents(void);
int npcs_snapshot(uint8_t *b, int max);
void npcs_apply_snapshot(const uint8_t *b, int len);
static bool sram_ok(void) { static int ok = -1; if (ok < 0) { sram_init(); ok = sram_detect() > 0; } return ok; }
static struct { bool valid; uint32_t serial; char map[32]; } save_dir[NUM_SAVE_SLOTS];

// read every slot header (at launch and after each write)
void game_saves_scan(void)
{
    bool ok = sram_ok();
    for (int k = 0; k < NUM_SAVE_SLOTS; k++) {
        save_dir[k].valid = false;
        if (!ok) continue;
        struct { uint32_t magic, serial; char map[32]; } h __attribute__((aligned(16)));
        sram_read(&h, k * SAVE_SLOT_BYTES, sizeof(h));
        if (h.magic != SAVE_MAGIC) continue;
        save_dir[k].valid = true;
        save_dir[k].serial = h.serial;
        memcpy(save_dir[k].map, h.map, 32);
        save_dir[k].map[31] = 0;
    }
}
// for the load menu: newest first. returns count; slots[i], labels[i]
int game_saves_list(int *slots, char (*labels)[48])
{
    int n = 0;
    for (int k = 0; k < NUM_SAVE_SLOTS; k++) if (save_dir[k].valid) slots[n++] = k;
    for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++)
        if (save_dir[slots[b]].serial > save_dir[slots[a]].serial) { int t = slots[a]; slots[a] = slots[b]; slots[b] = t; }
    for (int a = 0; a < n; a++)
        snprintf(labels[a], 48, "%s - %s", slots[a] == 0 ? "Autosave" : "Saved game", save_dir[slots[a]].map);
    return n;
}
bool game_save_exists(void)
{
    for (int k = 0; k < NUM_SAVE_SLOTS; k++) if (save_dir[k].valid) return true;
    return false;
}
static void write_slot(int slot, const float *o, const float *a)
{
    if (!sram_ok()) { hud_message("No save memory"); return; }
    uint32_t serial = 0;
    for (int k = 0; k < NUM_SAVE_SLOTS; k++) if (save_dir[k].valid && save_dir[k].serial > serial) serial = save_dir[k].serial;
    memset(&save_buf, 0, sizeof(save_buf));
    save_buf.s.magic = SAVE_MAGIC;
    save_buf.s.serial = serial + 1;
    strncpy(save_buf.s.map, cur_map, 31);
    memcpy(save_buf.s.o, o, 12);
    memcpy(save_buf.s.a, a, 12);
    save_buf.s.skill = settings.skill;
    save_buf.s.pl = player;
    // level snapshot: [u16 ent bytes][ents][u16 npc bytes][npcs]
    int base = npcs_base_ents(), pos = SAVE_HDR;
    int ne = ents_snapshot(save_buf.raw + pos + 2, SAVE_SLOT_BYTES - pos - 2 - 2400, base);
    save_buf.raw[pos] = ne & 255; save_buf.raw[pos + 1] = ne >> 8; pos += 2 + ne;
    int nn = npcs_snapshot(save_buf.raw + pos + 2, SAVE_SLOT_BYTES - pos - 2);
    save_buf.raw[pos] = nn & 255; save_buf.raw[pos + 1] = nn >> 8;
    sram_write(save_buf.raw, slot * SAVE_SLOT_BYTES, SAVE_SLOT_BYTES);
    game_saves_scan();
}
static void autosave(void) { write_slot(0, arrive_origin, arrive_angles); }
// pause menu "Save game": an empty manual slot, else the oldest one
void game_save_manual(void)
{
    if (!in_game) return;
    int best = 1;
    for (int k = 1; k < NUM_SAVE_SLOTS; k++) {
        if (!save_dir[k].valid) { best = k; break; }
        if (save_dir[k].serial < save_dir[best].serial) best = k;
    }
    float o[3];
    pm_get_origin(o);
    write_slot(best, o, view_angles);
    hud_message("Game saved");
}
static bool load_save(int slot)
{
    if (slot < 0) {   // newest
        uint32_t s = 0;
        for (int k = 0; k < NUM_SAVE_SLOTS; k++) if (save_dir[k].valid && save_dir[k].serial >= s) { s = save_dir[k].serial; slot = k; }
    }
    if (slot < 0 || slot >= NUM_SAVE_SLOTS || !save_dir[slot].valid) return false;
    sram_read(save_buf.raw, slot * SAVE_SLOT_BYTES, SAVE_SLOT_BYTES);
    save_t sv = save_buf.s;
    num_visited = 0; noclip = false; end_requested = false;
    player_new_game();
    snd_music_stop();
    settings.skill = sv.skill;
    in_game = load_map(sv.map, NULL, NULL);
    autosave_pending = false;   // don't overwrite the autosave just by loading
    if (!in_game) return false;
    player = sv.pl;
    {
        int pos = SAVE_HDR, ne = save_buf.raw[pos] | (save_buf.raw[pos + 1] << 8);
        if (pos + 2 + ne <= SAVE_SLOT_BYTES) {
            ents_apply_snapshot(save_buf.raw + pos + 2, ne, npcs_base_ents());
            pos += 2 + ne;
            int nn = save_buf.raw[pos] | (save_buf.raw[pos + 1] << 8);
            if (pos + 2 + nn <= SAVE_SLOT_BYTES) npcs_apply_snapshot(save_buf.raw + pos + 2, nn);
        }
    }
    pm_reset_player(sv.o);
    memcpy(view_angles, sv.a, 12);
    memcpy(arrive_origin, sv.o, 12);
    memcpy(arrive_angles, sv.a, 12);
    hud_message("Game loaded");
    return true;
}

static bool debug_load;   // the Debug menu's map loader
static bool start_game(const char *map)
{
    if (!strncmp(map, "*save", 5)) return load_save(map[5] ? atoi(map + 5) : -1);
    num_visited = 0;
    noclip = false;
    player_new_game();
    snd_music_stop();
    end_requested = false;
    arrive_t arr;
    debug_arrive = (debug_load && find_arrive(map, &arr)) ? &arr : NULL;
    debug_load = false;
    in_game = load_map(map, NULL, NULL);
    debug_arrive = NULL;
    return in_game;
}

static void leave_game(void)
{
    in_game = false;
    snd_stop_all();
    snd_music_stop();
    hud_reset();
    ents_player_teleported();
    menu_open(false);
}

void game_autosave_tick(void) { if (autosave_pending && in_game) { autosave_pending = false; autosave(); hud_message("Autosaved"); } }

static void do_pending_change(void)
{
    pending_change = false;
    if (!map_in_rom(pending_map)) {
        char b[96];
        snprintf(b, sizeof(b), "%s is not in this ROM", pending_map);
        hud_message(b);
        return;
    }
    float oldlm[3] = {0, 0, 0};
    bool have = false;
    for (int i = ents_find_class("info_landmark", 0); i >= 0; i = ents_find_class("info_landmark", i + 1)) {
        if (!strcasecmp(entities[i].targetname, pending_landmark)) {
            memcpy(oldlm, entities[i].origin, 12);
            have = true;
            break;
        }
    }
    if (have) { ents_save_globals(oldlm); npcs_save_transition(pending_landmark, oldlm); }
    char map[32], lm[48];
    strcpy(map, pending_map);
    strcpy(lm, pending_landmark);
    load_map(map, have ? lm : NULL, have ? oldlm : NULL);
}

// after a death: back to how the player entered this map (the PC loads the autosave)
static void restart_map(void)
{
    char map[32];
    float o[3], a[3];
    strcpy(map, cur_map);
    memcpy(o, arrive_origin, 12);
    memcpy(a, arrive_angles, 12);
    player_restore_map_start();
    hud_reset();
    load_map(map, NULL, NULL);
    pm_reset_player(o);
    memcpy(view_angles, a, 12);
    memcpy(arrive_origin, o, 12);
    memcpy(arrive_angles, a, 12);
}

// ------------------------------------------------------------------ HUD
static bool paused;

static float hud_dt;
static void draw_hud(void);
// GoldSrc level change: the last frame stays on screen with "LOADING" over it
bool loading_overlay_active;
sprite_t *loading_lambda;   // loaded at boot
static void draw_hud_loading(void)
{
    draw_hud();
    font_begin(BLEND_ALPHA);
    font_color(RGBA32(255, 160, 0, 255));
    font_draw_centered(FONT_LARGE, 160, 112, "LOADING");
    // the lambda just left of the text, exactly as tall as the letters' ink
    extern sprite_t *loading_lambda;
    sprite_t *lam = loading_lambda;
    int t0, b0;
    if (lam && font_ink_rows(FONT_LARGE, 'L', &t0, &b0)) {
        int h = b0 - t0 + 1, w = font_width(FONT_LARGE, "LOADING");
        float sc = (float)h / lam->height;
        rdpq_sync_pipe();
        rdpq_set_mode_standard();
        rdpq_mode_alphacompare(1);
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_sprite_blit(lam, 160 - w / 2 - 4 - lam->width * sc, 112 + t0, &(rdpq_blitparms_t){.scale_x = sc, .scale_y = sc});
    }
}
static void draw_hud(void)
{
#ifndef NOHUD
    if (cam_ent < 0 && !paused) {
        player_draw_hud(hud_dt);
        weapons_draw_hud(hud_dt, true);   // crosshair is part of the HEV HUD
    }
#endif
    hud_draw_fade();
    hud_draw_messages();
    if (cam_ent < 0) hud_draw_train();
    if (player.dead && player_dead_time() > 2.5f) {
        font_begin(BLEND_ALPHA);
        font_color(RGBA32(255, 160, 0, 255));
        font_draw_centered(FONT_SMALL, 160, 200, "Press A to restart");
    }
    if (noclip || settings.show_stats) {
        font_begin(BLEND_ALPHA);
        font_color(RGBA32(255, 160, 0, 255));
        char b[96];
        if (noclip) font_draw(FONT_SMALL, 8, 222, "noclip");
        if (settings.show_stats) {
            float o[3];
            pm_get_origin(o);
            snprintf(b, sizeof(b), "%s  %.1f fps  %d tris  %d chars", world.name, fps_value, rstats.tris, rstats.npcs);
            font_draw(FONT_SMALL, 8, 8, b);
            snprintf(b, sizeof(b), "%.0f %.0f %.0f", o[0], o[1], o[2]);
            font_draw(FONT_SMALL, 8, 19, b);
        }
    }
    if (paused) menu_draw();
}

// ------------------------------------------------------------------ input
static float axis(int v, float dead)
{
    float f = v / 80.0f;
    if (fabsf(f) < dead) return 0;
    f = (f > 0 ? f - dead : f + dead) / (1 - dead);
    if (f > 1) f = 1;
    if (f < -1) f = -1;
    return f;
}

static void build_input(float dt, joypad_inputs_t in, joypad_buttons_t pressed)
{
    memset(&input, 0, sizeof(input));
    float sx = axis(in.stick_x, 0.12f), sy = axis(in.stick_y, 0.12f);
    float fwd = 0, side = 0;
    const float speed = 400;                       // cl_forwardspeed / cl_sidespeed
    float sens = settings.look_sens / 5.0f;
    float inv = settings.invert_y ? -1.0f : 1.0f;
    switch (settings.control_scheme) {
    case 0: {
        // stick = move (analog), C buttons = look (cl_yawspeed 210, cl_pitchspeed 225, with a
        // short ramp so taps can aim precisely), D-pad = move
        fwd = sy * speed;
        side = sx * speed;
        bool turning = in.btn.c_left || in.btn.c_right || in.btn.c_up || in.btn.c_down;
        turn_held = turning ? turn_held + dt : 0;
        float ramp = fminf(1.0f, 0.35f + turn_held * 2.5f);
        if (in.btn.c_left) view_angles[1] += 210.0f * sens * ramp * dt;
        if (in.btn.c_right) view_angles[1] -= 210.0f * sens * ramp * dt;
        if (in.btn.c_up) view_angles[0] -= inv * 225.0f * sens * ramp * dt;
        if (in.btn.c_down) view_angles[0] += inv * 225.0f * sens * ramp * dt;
        break;
    }
    case 1: {
        // stick = look (accelerating curve), C buttons / d-pad = move
        float yawrate = (200.0f * sx * fabsf(sx) + 40.0f * sx) * sens;
        float pitchrate = (140.0f * sy * fabsf(sy) + 30.0f * sy) * sens;
        view_angles[1] -= yawrate * dt;
        view_angles[0] -= inv * pitchrate * dt;
        if (in.btn.c_up) fwd += speed;
        if (in.btn.c_down) fwd -= speed;
        if (in.btn.c_left) side -= speed;
        if (in.btn.c_right) side += speed;
        break;
    }
    default:
        // GoldenEye 1.1: stick = forward/back + turn, C = look up/down + strafe
        view_angles[1] -= (180.0f * sx * fabsf(sx) + 30.0f * sx) * sens * dt;
        fwd = sy * speed;
        if (in.btn.c_up) view_angles[0] -= inv * 120.0f * sens * dt;
        if (in.btn.c_down) view_angles[0] += inv * 120.0f * sens * dt;
        if (in.btn.c_left) side -= speed;
        if (in.btn.c_right) side += speed;
        break;
    }
    if (view_angles[0] > 89) view_angles[0] = 89;
    if (view_angles[0] < -89) view_angles[0] = -89;
    view_angles[1] = fmodf(view_angles[1] + 360.0f, 360.0f);
    // Z fires, R is the secondary attack, L ducks; the D-pad picks weapons (left/right),
    // reloads (down) and switches the flashlight (up)
    if (in.btn.a) input.buttons |= IN_JUMP;
    if (in.btn.l) input.buttons |= IN_DUCK;
    {   // PS2-style crouch-jump: keep A held after jumping and the legs tuck up in the air
        static float a_held;
        extern int pm_get_groundent(void);
        a_held = in.btn.a ? a_held + dt : 0;
        extern int pm_get_waterlevel(void);
        if (a_held > 0.2f && pm_get_groundent() < 0 && !noclip && pm_get_waterlevel() < 2) input.buttons |= IN_DUCK;
    }
    if (in.btn.b) input.buttons |= IN_USE;
    weapon_fire = in.btn.z;
    weapon_fire2 = in.btn.r;
    weapon_reload = pressed.d_down;
    weapon_select = pressed.d_right ? 1 : (pressed.d_left ? -1 : 0);
    if (pressed.d_up && !in.btn.l) player_toggle_flashlight();
    // driving a train: forward/back change speed in steps, a deliberate sidestep gets off
    if (ents_player_driving()) {
        if (fwd > speed * 0.5f) input.buttons |= IN_FORWARD;
        if (fwd < -speed * 0.5f) input.buttons |= IN_BACK;
        if (side < -speed * 0.75f) input.buttons |= IN_MOVELEFT;
        if (side > speed * 0.75f) input.buttons |= IN_MOVERIGHT;
        fwd = side = 0;
        input.buttons &= ~(IN_JUMP | IN_DUCK);
    } else {
        if (fwd > 0) input.buttons |= IN_FORWARD;
        if (fwd < 0) input.buttons |= IN_BACK;
        if (side < 0) input.buttons |= IN_MOVELEFT;
        if (side > 0) input.buttons |= IN_MOVERIGHT;
    }
    input.forward = fwd;
    input.side = side;
    {   // the wished move in world space (pushables use it: a cart blocks the player's velocity)
        extern float g_wish_move[2];
        float yr = view_angles[1] * (float)M_PI / 180;
        g_wish_move[0] = cosf(yr) * fwd + sinf(yr) * side;
        g_wish_move[1] = sinf(yr) * fwd - cosf(yr) * side;
    }
    if (noclip) {
        if (in.btn.a) input.up += speed;
        if (in.btn.l) input.up -= speed;
    }
    // cheats: L + D-up toggles noclip, L + D-down gives every weapon (impulse 101)
    if (settings.cheats && in.btn.l && pressed.d_up) noclip = !noclip;
    if ((settings.cheats && in.btn.l && pressed.d_down) || dbg_give_all) {
        dbg_give_all = false;
        static const char *all[] = {"item_suit", "weapon_crowbar", "weapon_9mmhandgun", "weapon_357", "weapon_9mmAR", "weapon_shotgun",
                                    "weapon_crossbow", "weapon_rpg", "weapon_gauss", "weapon_egon", "weapon_hornetgun",
                                    "weapon_handgrenade", "weapon_tripmine", "weapon_satchel", "weapon_snark"};
        player.suit = true;
        player.weapons |= 1u << WEAPON_SUIT;
        for (unsigned i = 1; i < sizeof(all) / sizeof(all[0]); i++) { weapons_give(all[i]); weapons_give(all[i]); }
        for (int k = 0; k < 5; k++) { weapons_give_ammo("ammo_9mmbox"); weapons_give_ammo("ammo_ARgrenades"); weapons_give_ammo("ammo_buckshot");
            weapons_give_ammo("ammo_357"); weapons_give_ammo("ammo_crossbow"); weapons_give_ammo("ammo_rpgclip"); weapons_give_ammo("ammo_gaussclip"); }
        player.armor = 100;
        weapon_reload = false;
        debugf("cheat: all weapons\n");
    }
    if (dbg_noclip != dbg_noclip_prev) { noclip = dbg_noclip; dbg_noclip_prev = dbg_noclip; }
    if (!settings.cheats && !dbg_noclip) noclip = false;
    input.noclip = noclip;
    memcpy(input.viewangles, view_angles, 12);
}

// ------------------------------------------------------------------ view (cl_dll/view.cpp V_CalcBob/V_CalcRoll)
static float calc_bob(const float *vel, float t)
{
    const float cl_bob = 0.01f, cl_bobcycle = 0.8f, cl_bobup = 0.5f;
    float cycle = t - (int)(t / cl_bobcycle) * cl_bobcycle;
    cycle /= cl_bobcycle;
    if (cycle < cl_bobup) cycle = M_PI * cycle / cl_bobup;
    else cycle = M_PI + M_PI * (cycle - cl_bobup) / (1.0f - cl_bobup);
    float bob = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]) * cl_bob;
    bob = bob * 0.3f + bob * 0.7f * sinf(cycle);
    if (bob > 4) bob = 4;
    if (bob < -7) bob = -7;
    return bob;
}

static float calc_roll(const float *angles, const float *vel)
{
    const float rollangle = 2.0f, rollspeed = 200.0f;
    float y = angles[1] * M_PI / 180;
    float right[3] = {sinf(y), -cosf(y), 0};
    float side = vel[0] * right[0] + vel[1] * right[1];
    float sign = side < 0 ? -1 : 1;
    side = fabsf(side);
    side = side < rollspeed ? side * rollangle / rollspeed : rollangle;
    return side * sign;
}

// trigger_camera view: from the camera entity, turned towards its target (CTriggerCamera::FollowTarget)
static void camera_view(float dt, float *eye, float *ang)
{
    entity_t *c = &entities[cam_ent];
    memcpy(eye, c->origin, 12);
    float tgt[3];
    bool have = false;
    int t = c->target[0] ? ents_find_targetname(c->target, 1) : -1;
    if (t >= 0) {
        if (npc_origin_for_entity(t, tgt)) have = true;
        else {
            memcpy(tgt, entities[t].origin, 12);
            if (entities[t].model > 0) {
                bmodel_t *bm = &world.bmodels[entities[t].model];
                for (int k = 0; k < 3; k++) tgt[k] += (bm->mins[k] + bm->maxs[k]) * 0.5f;
            }
            have = true;
        }
    }
    if (have) {
        float d[3] = {tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]};
        float goal[3] = {-atan2f(d[2], sqrtf(d[0] * d[0] + d[1] * d[1])) * 180 / M_PI, atan2f(d[1], d[0]) * 180 / M_PI, 0};
        if (cam_snap) { memcpy(cam_angles, goal, 12); cam_snap = false; }
        for (int k = 0; k < 2; k++) {
            float dd = fmodf(goal[k] - cam_angles[k] + 540.0f, 360.0f) - 180.0f;
            cam_angles[k] += dd * fminf(1.0f, 2.5f * dt);
        }
    } else if (cam_snap) {
        memcpy(cam_angles, c->angles, 12);
        cam_snap = false;
    }
    memcpy(ang, cam_angles, 12);
}

// ------------------------------------------------------------------ scripted input for automated tests
#ifdef TESTKEYS
// "time:key[:hold]" separated by spaces; keys: A B Z S(start) R L DU DD DL DR CU CD CL CR SU SD SL SR
static void test_keys(float t, joypad_inputs_t *in, joypad_buttons_t *pressed)
{
    static const char *script = TESTKEYS;
    static float last_t;
    const char *p = script;
    while (*p) {
        float at, hold = 0.1f;
        char key[4] = {0};
        int n = 0;
        if (sscanf(p, "%f:%3[A-Z]%n", &at, key, &n) < 2) break;
        p += n;
        if (*p == ':') { int m = 0; sscanf(p + 1, "%f%n", &hold, &m); p += 1 + m; }
        while (*p == ' ') p++;
        bool down = t >= at && t < at + hold;
        bool edge = last_t < at && t >= at;
#define K(name, field) if (!strcmp(key, name)) { if (down) in->btn.field = 1; if (edge) pressed->field = 1; }
        K("A", a) K("B", b) K("Z", z) K("S", start) K("R", r) K("L", l)
        K("DU", d_up) K("DD", d_down) K("DL", d_left) K("DR", d_right)
        K("CU", c_up) K("CD", c_down) K("CL", c_left) K("CR", c_right)
#undef K
        if (down && !strcmp(key, "SU")) in->stick_y = 80;
        if (down && !strcmp(key, "SD")) in->stick_y = -80;
        if (down && !strcmp(key, "SL")) in->stick_x = -80;
        if (down && !strcmp(key, "SR")) in->stick_x = 80;
    }
    last_t = t;
}
#endif

// ------------------------------------------------------------------ main
#ifdef WATCHDOG
static volatile int wd_frames, wd_last, wd_stall;
static void wd_cb(int ovfl)
{
    (void)ovfl;
    if (wd_frames == wd_last) { if (++wd_stall >= 3) { debugf("WATCHDOG: main loop stalled\n"); assertf(0, "watchdog"); } }
    else wd_stall = 0;
    wd_last = wd_frames;
}
#endif
#define TR(x) do { if (trace_frames > 0) debugf("%s\n", x); } while (0)
int main(void)
{
    debug_init_isviewer();
    debug_init_usblog();
    dfs_init(DFS_DEFAULT_LOCATION);
    asset_init_compression(2);
    asset_init_compression(3);   // maps are Shrinkler-compressed
    joypad_init();
    render_init();
    snd_init();
    pm_init();
    hud_init();
    skill_init();
    player_new_game();
    weapons_hud_init();   // HUD sprites stay loaded: allocate them before any map
    loading_lambda = sprite_load("rom:/ui/lambda.sprite");
    {   // newlib allocates FILE structs in blocks the first time many files are open at once and
        // never frees them: open a dozen now so that block sits at the bottom of the heap instead
        // of splitting it in the middle of a map (which left later maps without room to load)
        FILE *fs[12];
        for (int i = 0; i < 12; i++) fs[i] = fopen("rom:/titles.txt", "rb");
        for (int i = 0; i < 12; i++) if (fs[i]) fclose(fs[i]);
    }
    {   // printf of floats allocates newlib's big-number cache on first use and keeps it forever;
        // do it now so it isn't left in the middle of a map's memory (it split the heap)
        char tmp[64];
        volatile float vals[] = {0.5f, 123.456f, 1e-7f, 98765.4321f, 3.0e8f, -0.001f};
        for (int i = 0; i < 6; i++) { snprintf(tmp, sizeof(tmp), "%f %.1f %e %g", vals[i], vals[i], vals[i], vals[i]); (void)tmp; }
    }
    game_saves_scan();
    menu_init();
    load_maplist();
    debugf("HL64 boot: %d maps, heap %d KB\n", num_maps, (int)(get_memory_size() / 1024));
#ifdef RDPVALIDATE
    rdpq_debug_start();
#endif
    menu_open(false);

#ifdef AUTOSTART_MAP
    menu_close();
#ifdef DEBUGWARP
    debug_load = true;
#endif
    start_game(AUTOSTART_MAP);
    settings.show_stats = true;
    settings.cheats = true;
#ifdef TESTPOS
    { float tp[5] = {TESTPOS}; pm_reset_player(tp); view_angles[1] = tp[3]; view_angles[0] = tp[4]; }
#ifdef TESTDUCK
    { extern void pm_set_ducked(void); pm_set_ducked(); }
#endif
#ifdef TESTNOCLIP
    settings.cheats = true; dbg_noclip = true; dbg_god = true; player.suit = true; dbg_give_all = true;
#endif
#endif
#endif

#ifdef WATCHDOG
    timer_init();
    new_timer(TIMER_TICKS(1000000), TF_CONTINUOUS, wd_cb);
#endif
    uint64_t last = get_ticks();
    float fps_acc = 0; int fps_frames = 0;
    for (;;) {
#ifdef WATCHDOG
        wd_frames++;
#endif
        TR("T"); uint64_t now = get_ticks();
        float dt = TICKS_DISTANCE(last, now) / (float)TICKS_PER_SECOND;
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        if (dt < 0.001f) dt = 0.001f;
        game_clock += dt;
        fps_acc += dt; fps_frames++;
        if (fps_acc >= 1.0f) {
            fps_value = fps_frames / fps_acc;
            float po[3]; pm_get_origin(po);
            debugf("t=%.1f ai %.1fms fps %.1f tris %d ents %d npcs %d leaf %d pos %.0f %.0f %.0f yaw %.0f%s\n", game_clock, TICKS_TO_US(prof_npc) / 1000.0f / (fps_frames ? fps_frames : 1), fps_value, rstats.tris,
                   rstats.ents, rstats.npcs, rstats.leaf, po[0], po[1], po[2], view_angles[1],
                   in_game ? "" : " (menu)");
            {   // per-frame CPU milliseconds: game logic, then the renderer's parts (wait = for a free framebuffer)
                float k = 1.0f / 1000.0f / (fps_frames ? fps_frames : 1);
                debugf("  prof lights %.1f gtb %.1f ents %.1f npc %.1f pm %.1f play %.1f | rend %.1f: vis %.1f wait %.1f sky %.1f world %.1f mdl %.1f (brush %.1f npc %.1f)\n",
                       TICKS_TO_US(prof_lights) * k, TICKS_TO_US(prof_gtb) * k, TICKS_TO_US(prof_ents) * k, TICKS_TO_US(prof_npc) * k, TICKS_TO_US(prof_pm) * k, TICKS_TO_US(prof_play) * k,
                       TICKS_TO_US(prof_rend) * k, TICKS_TO_US(rstats.t_vis) * k, TICKS_TO_US(rstats.t_wait) * k, TICKS_TO_US(rstats.t_sky) * k,
                       TICKS_TO_US(rstats.t_world) * k, TICKS_TO_US(rstats.t_ents) * k, TICKS_TO_US(rstats.t_bents) * k, TICKS_TO_US(rstats.t_npcs) * k);
                prof_lights = prof_gtb = prof_ents = prof_pm = prof_play = prof_rend = 0;
                rstats.t_vis = rstats.t_wait = rstats.t_sky = rstats.t_world = rstats.t_ents = rstats.t_bents = rstats.t_npcs = 0;
            }
            fps_acc = 0; fps_frames = 0; prof_npc = 0;
        }

        TR("j"); joypad_poll(); TR("J");
        joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
        joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
#ifdef TESTKEYS
        test_keys(game_clock, &in, &pressed);
#endif
#ifdef MAPCYCLE
        // automated test: load every map in the ROM in turn and render it from a few angles
        {
#ifndef MAPCYCLE_START
#define MAPCYCLE_START 0
#endif
            static int cyc = MAPCYCLE_START - 1; static float next;
            if (game_clock >= next) {
                cyc++;
                if (cyc >= num_maps) { debugf("MAPCYCLE DONE\n"); cyc = 0; }
                heap_stats_t hs; sys_get_heap_stats(&hs);
                debugf("MAPCYCLE %d %s heap used %d KB of %d KB\n", cyc, maplist[cyc], hs.used / 1024, hs.total / 1024);
                menu_close();
                start_game(maplist[cyc]);
                player.suit = true; weapons_give("weapon_crossbow");
                dbg_give_all = true; dbg_god = true;
                player.flashlight = true;
                next = game_clock + 3.5f;
            }
            memset(&in, 0, sizeof(in)); memset(&pressed, 0, sizeof(pressed));
            view_angles[1] = fmodf(game_clock * 90.0f, 360.0f);
            // exercise effects: fire, gibs, blood, a save and a load
            {
                float ph = fmodf(game_clock, 3.5f);
                in.btn.z = ph > 1.0f && ph < 2.0f;
                static int stage = -1;
                int st = (int)(ph * 2);
                if (st != stage) {
                    stage = st;
                    float o[3]; pm_get_origin(o);
                    o[0] += 64;
                    extern void game_spawn_gibs(const float *o, int count, bool alien, float speed);
                    if (st == 3) { game_spawn_gibs(o, 8, cyc & 1, 200); debugf("TEST gibs\n"); }
                    if (st == 4 && (cyc % 5) == 0) { game_save_manual(); debugf("TEST save\n"); }
                    if (st == 5 && (cyc % 10) == 0) { debugf("TEST load\n"); load_save(-1); }
                    if (st == 6) { pressed.d_right = 1; }
                }
            }
        }
#endif
#ifdef TESTFIRE
        { static bool done; if (!done && in_game && game_clock > 3) { done = true; debugf("TESTFIRE %s\n", TESTFIRE); ents_fire_targets(TESTFIRE); } }
        { static float nx; if (in_game && game_clock > nx) { nx = game_clock + 0.5f;
            for (int i = 0; i < num_npcs; i++) if (entities[npcs[i].ent].targetname[0]) debugf("NPC %s %.0f %.0f %.0f hp %.0f ai %d script %d %s\n", entities[npcs[i].ent].targetname, npcs[i].origin[0], npcs[i].origin[1], npcs[i].origin[2], npcs[i].health, npcs[i].ai, npcs[i].script, entities[npcs[i].ent].removed ? "GONE" : ""); } }
#endif
#ifdef TESTDMG
        { static float nt = 4; static int k; if (in_game && game_clock > nt && k < 4) { nt = game_clock + 0.5f; k++;
            for (int i = 0; i < num_npcs; i++) {
                entity_t *e = &entities[npcs[i].ent];
                if (e->removed) continue;
                bool corpse = strstr(e->classname, "_dead") != NULL;
                npc_take_damage(i, corpse ? 10 : 60, DMG_CLUB, NULL, 0);
                debugf("TESTDMG %d %s hit %d -> health %.0f %s\n", i, e->classname, k, npcs[i].health, e->removed ? "GIBBED" : "");
            } } }
#endif
#ifdef TESTFX
        {   // effects test: explosions in front, then the gluon gun firing
            static float nt = 3; static int k;
            extern void game_effect_explosion(const float *o, float m);
            extern void weapons_test_select(int id);
            if (in_game && game_clock > nt) {
                nt = game_clock + 1.0f; k++;
                float y = view_angles[1] * (float)M_PI / 180;
                float o[3]; pm_get_origin(o);
                o[0] += cosf(y) * 200; o[1] += sinf(y) * 200;
                if (k == 1) { dbg_give_all = true; dbg_god = true; }
                if (k >= 2 && k <= 4) { game_effect_explosion(o, 120); debugf("TESTFX explosion\n"); }
                if (k == 5) { weapons_test_select(4); debugf("TESTFX mp5\n"); }
                if (k == 7) { weapons_test_select(10); debugf("TESTFX egon\n"); }
            }
            if (k >= 5) in.btn.z = 1;
        }
#endif
#ifdef TESTCHANGE
        { static bool done; if (!done && in_game && game_clock > 4) { done = true; game_request_changelevel(TESTCHANGE); } }
#endif
        TR("V"); snd_set_volume(settings.volume / 10.0f);
        {
            float y = last_ang[1] * M_PI / 180;
            float right[3] = {sinf(y), -cosf(y), 0};
            TR("S"); snd_update(last_eye, right); TR("s");
        }

        // ---- title screen
        if (!in_game) {
            char map[32] = "";
            menu_action_t act = menu_update(dt, in, pressed, map, sizeof(map));
            if (act == MENU_ACT_START_MAP) { debug_load = map[0] != '*' && menu_debug_warped(); start_game(map); continue; }
            if (!menu_is_open()) menu_open(false);
            rdpq_attach_clear(display_get(), NULL);
            menu_draw();
            rdpq_detach_show();
            continue;
        }

        // ---- pause menu over the frozen game
        if (menu_is_open()) {
            char map[32] = "";
            menu_action_t act = menu_update(dt, in, pressed, map, sizeof(map));
            if (act == MENU_ACT_START_MAP) { menu_close(); debug_load = map[0] != '*' && menu_debug_warped(); start_game(map); continue; }
            if (act == MENU_ACT_LEAVE) { leave_game(); continue; }
            paused = menu_is_open();
            if (paused) { render_frame(last_eye, last_ang, draw_hud); continue; }
        }
        paused = false;
        if (pressed.start) { menu_open(true); paused = true; render_frame(last_eye, last_ang, draw_hud); continue; }

        // ---- game tick
        hud_update(dt);
        if (title_delay > 0 && (title_delay -= dt) <= 0) hud_show_message(title_name);
        if (cam_ent >= 0 && cam_freeze) { memset(&in, 0, sizeof(in)); memset(&pressed, 0, sizeof(pressed)); }
        build_input(dt, in, pressed);
        int newly = input.buttons & ~prev_buttons;
        prev_buttons = input.buttons;

        float origin[3], vofs[3], vel[3], punch[3];
        pm_get_origin(origin);
        pm_get_view_ofs(vofs);
        float eye0[3] = {origin[0] + vofs[0], origin[1] + vofs[1], origin[2] + vofs[2]};
        if ((input.buttons & IN_USE) && player_alive()) {
            // +use: take or leave a train's controls, otherwise use what's in front of us
            // (chargers keep working while it is held)
            if (!(newly & IN_USE) || !ents_player_train_use()) {
                float y = view_angles[1] * M_PI / 180, p = view_angles[0] * M_PI / 180;
                float fwd[3] = {cosf(p) * cosf(y), cosf(p) * sinf(y), -sinf(p)};
                ents_player_use_hold(eye0, fwd, (newly & IN_USE) != 0);
            }
        }
        hud_set_train(ents_player_train_update(input.buttons, newly));

        if (player.dead) {
            // dead: no moving; a button press after a moment starts the map again
            input.forward = input.side = input.up = 0;
            input.buttons = 0;
            if (player_dead_time() > 2.5f && (pressed.a || pressed.b || pressed.z)) { restart_map(); continue; }
        }
        hud_dt = dt;
        pm_clear_basevelocity();
        uint32_t te0 = TICKS_READ();
        TR("E"); ents_think(dt);
        uint32_t te1 = TICKS_READ();
        { extern void lights_think(float dt); lights_think(dt); }
        prof_ents += TICKS_DISTANCE(te0, te1); prof_lights += TICKS_DISTANCE(te1, TICKS_READ());
#ifndef NONPC
        TR("N"); uint32_t tn0 = TICKS_READ(); game_trace_begin_frame(); prof_gtb += TICKS_DISTANCE(tn0, TICKS_READ()); npcs_think(dt); prof_npc += TICKS_DISTANCE(tn0, TICKS_READ());
#endif
        uint32_t tp0 = TICKS_READ();
        TR("P"); pm_run(&input, dt);
        pm_unstick(dt);
        uint32_t tp1 = TICKS_READ();
        TR("L"); player_think(dt); game_autosave_tick(); TR("W");
        weapons_think(dt, view_angles, weapon_fire && cam_ent < 0, weapon_fire2 && cam_ent < 0, weapon_reload, weapon_select,
                      cam_ent < 0 && !ents_player_driving() && !noclip);
        prof_pm += TICKS_DISTANCE(tp0, tp1); prof_play += TICKS_DISTANCE(tp1, TICKS_READ());

        pm_get_origin(origin);
        pm_get_view_ofs(vofs);
        pm_get_velocity(vel);
        pm_get_punch(punch);
        float eye[3] = {origin[0] + vofs[0], origin[1] + vofs[1], origin[2] + vofs[2]};
        if (!noclip) eye[2] += calc_bob(vel, game_clock);
        float ang[3] = {view_angles[0] + punch[0], view_angles[1] + punch[1], view_angles[2] + punch[2]};
        ang[2] += calc_roll(view_angles, vel);
        if (player.dead) ang[2] = 80;    // V_CalcRefdef: the dead view rolls over
        if (shake_end > game_clock) {
            float a = shake_amp * (shake_end - game_clock) / (shake_dur > 0 ? shake_dur : 1);
            for (int k = 0; k < 3; k++) eye[k] += a * ((rand() % 200) - 100) / 100.0f * 0.5f;
            ang[2] += a * sinf(game_clock * shake_freq) * 0.5f;
        }
        if (cam_ent >= 0) camera_view(dt, eye, ang);
        memcpy(last_eye, eye, 12);
        memcpy(last_ang, ang, 12);

        uint32_t tr0 = TICKS_READ();
        TR("R"); render_frame(eye, ang, draw_hud); prof_rend += TICKS_DISTANCE(tr0, TICKS_READ()); TR("r\n"); if (trace_frames > 0) trace_frames--;
        if (hud_end_of_game() || end_requested) { end_requested = false; leave_game(); continue; }
        if (pending_change) {
            render_frame(eye, ang, draw_hud_loading);
            loading_overlay_active = true;
            do_pending_change();
            loading_overlay_active = false;
#ifdef TESTPOS2
            { float tp[5] = {TESTPOS2}; pm_reset_player(tp); view_angles[1] = tp[3]; view_angles[0] = tp[4]; }
#endif
        }
    }
}
