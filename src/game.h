// HL64 - shared game declarations
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MAX_ENTITIES 1024
#define SOLID_NOT_ 0
#ifndef SOLID_BSP
#define SOLID_BSP 4
#endif
#ifndef SOLID_NOT
#define SOLID_NOT 0
#endif

typedef enum {
    ENT_OTHER = 0, ENT_WORLD, ENT_WALL, ENT_ILLUSIONARY, ENT_DOOR, ENT_DOOR_ROTATING, ENT_BUTTON,
    ENT_ROT_BUTTON, ENT_PLAT, ENT_TRAIN, ENT_ROTATING, ENT_WATER, ENT_LADDER, ENT_BREAKABLE,
    ENT_WALL_TOGGLE, ENT_TRIGGER_ONCE, ENT_TRIGGER_MULTIPLE, ENT_TRIGGER_CHANGELEVEL,
    ENT_TRIGGER_TELEPORT, ENT_TRIGGER_RELAY, ENT_MULTI_MANAGER, ENT_TRIGGER_PUSH, ENT_TRIGGER_HURT,
    ENT_INVISIBLE_BRUSH, ENT_PUSHABLE, ENT_CONVEYOR, ENT_MOMENTARY_DOOR, ENT_TRACKTRAIN, ENT_TRIGGER_AUTO, ENT_TRACKAUTO, ENT_SCRIPTED, ENT_CHANGETARGET,
    ENT_PATH_TRACK, ENT_MULTISOURCE, ENT_ENV_FADE, ENT_ENV_MESSAGE, ENT_GAME_TEXT, ENT_CAMERA, ENT_SENTENCE,
    ENT_ENV_RENDER, ENT_ENDSECTION,
    ENT_AMBIENT, ENT_SPEAKER, ENT_CDAUDIO, ENT_TARGET_CDAUDIO, ENT_RECHARGE, ENT_HEALTHCHARGER,
    ENT_ENV_SHAKE, ENT_WEAPONSTRIP, ENT_ENV_SPARK, ENT_ENV_EXPLOSION, ENT_GLOBAL_BEAM, ENT_MONSTERMAKER,
} enttype_t;

enum { MS_IDLE = 0, MS_AT_BOTTOM, MS_AT_TOP, MS_GOING_UP, MS_GOING_DOWN };

typedef struct entity_s {
    const char *kv;        // start of this entity's block in the entity lump
    int maker_count, maker_live, maker_maxlive;   // monstermaker: left to make (-1 endless), alive, cap (-1 none)
    enttype_t type;
    char classname[32];
    char targetname[48];
    char target[48];
    int  model;            // inline model index (*N) or -1
    float origin[3], angles[3];
    float base_origin[3];
    float pos1[3], pos2[3], ang1[3], ang2[3];
    float movedir[3];
    float dest[3];          // move destination (linear) or angles (angular)
    bool  angular;
    float speed, wait, lip, delay, dist, health;
    float nextthink, ltime;
    float avelocity[3];
    int   spawnflags;
    int   state;
    int   rendermode, renderamt;
    int   solid;           // SOLID_BSP or SOLID_NOT
    int   contents;        // for func_water / func_ladder (moveents)
    bool  visible, removed, active, triggered;
    int   movesnd, stopsnd;
    float act_sign;
    // trains (func_train / func_tracktrain)
    int   path;            // current path_corner / path_track entity (tracktrain: m_ppath)
    float curspeed, height, length;
    // path_track links (CPathTrack::Link)
    int   pnext, pprev, palt;
    // tracktrain: placed origin and driver's control area (CFuncTrackTrain::SetControls)
    float oldorigin[3], ctlmins[3], ctlmaxs[3];
    float oldspeed;
    bool  deadend;
    // func_trackchange: tracks, carried train and state (CFuncTrackChange)
    int   tracktop, trackbottom, train, code, targetstate;
    // multisource / masters
    int   msource_count;
    float avel_cam[3];
    // sound / chargers / breakables
    int   snd;             // looping sound slot + 1 (0: none)
    float sndtime, juice, ltime2;
    int   charge_on;
    float maxhealth;
} entity_t;

extern entity_t entities[MAX_ENTITIES];
extern int num_entities;

typedef struct {
    float forward, side, up;
    float viewangles[3];
    int   buttons;
    bool  noclip;
} player_input_t;

// pmove.c
void pm_init(void);
void pm_run(player_input_t *in, float dt);
void pm_build_physents(void);
int  pm_entity_for_physent(int pe);
bool pm_player_in_entity(int ent, const float *pos);
bool pm_position_free(const float *pos);
bool pm_find_free_position(float *origin);
bool pm_unstick(float dt);

// entities.c
void ents_spawn_all(void);
void ents_think(float dt);
void ents_fire_targets(const char *name);
void ents_fire_entity_target(entity_t *e);
const char *ent_value(const entity_t *e, const char *key);
int  ents_find_class(const char *classname, int start);
int  ents_find_targetname(const char *name, int start);
float game_time(void);
void ents_save_globals(const float *landmark);
void ents_restore_globals(const float *landmark);

bool ents_player_train_use(void);                    // +use on a train's controls: start/stop driving
int  ents_player_train_update(int buttons, int pressed); // while driving: speed changes; HUD level or -1
bool ents_player_driving(void);
void ents_player_teleported(void);
bool ents_player_use_hold(const float *eye, const float *forward, bool pressed); // false: nothing usable
void ents_use_entity(int ent);                        // Use() on one entity (activator: the player)
void ents_damage_brush(int ent, float dmg, int dmgtype, const float *dir); // func_breakable / pushable
int  ents_trigger_hurt_touch(const float *mins, const float *maxs);   // damage the player takes this frame
bool ents_brush_is_breakable(int ent);

// main.c
void game_request_changelevel(const char *map, const char *landmark);
void hud_message(const char *msg);
void game_add_view_yaw(float dyaw);
void game_set_view_angles(const float *angles);
void game_set_camera(int camera_ent, float wait, bool freeze);   // trigger_camera (camera_ent < 0: off)
void game_end_section(void);                                     // back to the main menu
void game_shake(float amplitude, float frequency, float duration); // UTIL_ScreenShake on the player
void game_radius_damage(const float *origin, float damage, float radius, int attacker_npc, int dmgtype);
void game_effect_explosion(const float *origin, float magnitude);
void game_effect_debris(const float *origin, int material);
int  npc_spawn_item(const char *classname, const float *origin, float yaw);   // npc.c: items from crates etc.

// npc.c (helpers for entities)
bool npc_origin_for_entity(int ent, float *origin);
int  npc_for_entity(int ent);
int  npc_find_usable(const float *eye, const float *forward, float *bestdot);
void npc_player_use(int npc);
bool npc_can_speak(int npc, bool override, bool followers_only);
void npc_play_scripted_sentence(int npc, const char *sentence, float duration, float vol, float attn, bool concurrent, int listener_ent);
extern float talk_wait_time;   // CTalkMonster::g_talkWaitTime

// player.c
typedef struct {
    float health, armor;
    bool  suit, longjump, dead, flashlight;
    float flashbattery;
    uint32_t weapons;          // 1 << WEAPON_*
    int   ammo[16];
    int   clip[32];
    int   cur_weapon, last_weapon;
    float view_punch[3];
    float dmg_alpha, dmg_dir;
    int   air_finished_ms;     // drowning
    float pain_finished;
} player_t;
extern player_t player;
void player_new_game(void);
void player_map_start(void);           // snapshot for restarting after death
void player_restore_map_start(void);
bool player_take_health(float amount, int dmgtype);
bool player_take_armor(float amount);
void player_take_damage(float dmg, int dmgtype, const float *from, int attacker_ent);
bool player_touch_item(const char *classname, int npc_index);
void player_think(float dt);
void player_draw_hud(float dt);
void player_suit_update(const char *sentence, bool group, float nonrepeat_time);
bool player_alive(void);
void player_get_eye(float *eye);
void player_toggle_flashlight(void);
float player_dead_time(void);
#define DMG_GENERIC 0
#define DMG_CRUSH   (1 << 0)
#define DMG_BULLET  (1 << 1)
#define DMG_SLASH   (1 << 2)
#define DMG_BURN    (1 << 3)
#define DMG_FREEZE  (1 << 4)
#define DMG_FALL    (1 << 5)
#define DMG_BLAST   (1 << 6)
#define DMG_CLUB    (1 << 7)
#define DMG_SHOCK   (1 << 8)
#define DMG_SONIC   (1 << 9)
#define DMG_ENERGYBEAM (1 << 10)
#define DMG_NEVERGIB (1 << 12)
#define DMG_ALWAYSGIB (1 << 13)
#define DMG_DROWN   (1 << 14)
#define DMG_PARALYZE (1 << 15)
#define DMG_NERVEGAS (1 << 16)
#define DMG_POISON  (1 << 17)
#define DMG_RADIATION (1 << 18)
#define DMG_DROWNRECOVER (1 << 19)
#define DMG_ACID    (1 << 20)
#define DMG_SLOWBURN (1 << 21)
#define DMG_SLOWFREEZE (1 << 22)
#define DMG_MORTAR  (1 << 23)

// pmove.c player access
void  pm_get_origin(float *o);
void  pm_get_velocity(float *v);
void  pm_set_velocity(const float *v);
void  pm_get_punch(float *v);
void  pm_set_punch(const float *v);
void  pm_get_hull(float *mins, float *maxs);
int   pm_get_waterlevel(void);
float pm_get_fall_velocity(void);
bool  pm_noclip_active(void);
bool  pm_ducking(void);
int   pm_get_flags(void);
float player_max_health(void);

// skill.c (skill.cfg)
void  skill_init(void);
float skill_get(const char *name);    // "sk_plr_9mm_bullet" -> value for the current skill

// trace (pmove.c): hull traces against the world, brush entities, monsters and the player
typedef struct {
    float fraction;
    float endpos[3];
    float normal[3];
    bool  startsolid, allsolid;
    int   ent;      // entity index hit (0 = world, -1 = nothing)
    int   npc;      // npc index hit, or -1
    bool  player;   // hit the player
} gtrace_t;
#define TR_IGNORE_MONSTERS 1
#define TR_IGNORE_PLAYER   2
#define TR_IGNORE_GLASS    4
#define TR_WORLD_ONLY      8
// box: mins/maxs of the moving object relative to its origin (NULL = a point)
void game_trace(const float *start, const float *end, const float *mins, const float *maxs, int flags, int ignore_npc, gtrace_t *tr);
bool game_visible(const float *a, const float *b);
void game_trace_begin_frame(void);   // gather solid brush entities (call each frame after they move)   // line of sight (world + brushes)
int  game_point_contents(const float *p);

// sound.c  (GoldSrc channels / attenuations, as in common/const.h)
#ifndef CHAN_AUTO
#define CHAN_AUTO   0
#define CHAN_WEAPON 1
#define CHAN_VOICE  2
#define CHAN_ITEM   3
#define CHAN_BODY   4
#define CHAN_STREAM 5
#define CHAN_STATIC 6
#endif
#ifndef ATTN_NONE
#define ATTN_NONE   0
#define ATTN_NORM   0.8f
#define ATTN_IDLE   2.0f
#define ATTN_STATIC 1.25f
#endif
#ifndef PITCH_NORM
#define PITCH_NORM  100
#endif
void snd_init(void);
void snd_play(const char *sample, float volume, int channel);          // the player's own sounds
// ent: entity index the sound follows (0 = player / non-spatial, -1 = fixed at origin)
int  snd_emit(int ent, int chan, const char *sample, float vol, float attn, int pitch, const float *origin);
int  snd_sentence(int ent, const char *name, float vol, float attn, int pitch);  // "!NAME" or a group
int  snd_sentence_group_count(const char *group);
void snd_stop(int ent, int chan, const char *sample);     // chan -1: all; sample NULL: any
bool snd_playing(int ent, int chan);
bool snd_slot_playing(int id);
void snd_adjust(int id, float vol, int pitch);
void snd_update(const float *listener, const float *right);
void snd_stop_all(void);
void snd_poll(void);
void snd_set_volume(float v);
void snd_music_play(int track);
void snd_music_stop(void);
int  snd_music_track(void);
bool ents_sound_origin(int ent, float *out);                // entities.c: where an entity's sounds come from
