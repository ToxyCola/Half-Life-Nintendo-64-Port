// HL64 - monsters, NPCs and items.
//
// Follows the SDK's monster code where it matters for the game's scripted scenes and combat:
//   scripted.cpp    scripted_sequence: find the monster (retrying while it's busy), walk or run to
//                   the mark along the node graph, turn, play, fire the target when it's done
//   talkmonster.cpp scientists and Barney: +use to follow / stop following, greetings, idle chat,
//                   scripted_sentence speech, pre-disaster behaviour
//   monsters.cpp    a simplified schedule system: see an enemy, chase it along the node graph,
//                   melee / ranged attacks from the monster's own animations, pain and death
//   items.cpp       pickups (weapons, ammo, batteries, health, the HEV suit)
#include <libdragon.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include "bsp.h"
#include "game.h"
#include "studio.h"
#include "npc.h"
#include "nav.h"

npc_t npcs[MAX_NPCS];
int num_npcs;
float talk_wait_time;

void pm_get_origin(float *o);
void game_effect_tracer(const float *from, const float *to);
void game_effect_beam(const float *from, const float *to, int r, int g, int b);
int  proj_spawn(int kind, const float *origin, const float *dir, int owner, int target);

// ---------------------------------------------------------------- tables
enum { NS_IDLE = 0, NS_MOVING, NS_TURNING, NS_PLAYING, NS_SCRIPT_IDLE, NS_SCRIPT_WAIT };
enum { AI_IDLE = 0, AI_COMBAT, AI_ATTACK, AI_PAIN, AI_DYING, AI_DEAD, AI_FLEE, AI_GRABBED };

#define NF_NOTSOLID  1
#define NF_NODROP    2
#define NF_DEAD      4
#define NF_SITTING   8
#define NF_ITEM      16
#define NF_TALKER    32
#define NF_FLY       64
#define NF_STATIC    128
#define NF_NOAI      256
#define NF_INVULN    512
#define NF_SCI       1024
#define NF_PREDIS    2048
#define NF_GAG       4096
#define NF_PRISONER  8192
#define NF_WAITSEEN  16384

enum { C_NONE, C_ALLY, C_MILITARY, C_ALIEN, C_PREY, C_MACHINE };
enum { R_NONE, R_BULLET, R_ZAP, R_SONIC, R_LEAP, R_SPIT, R_HORNET, R_BALL, R_FLAME, R_SHOTGUN };

#define ACT_IDLE 1
#define ACT_WALK 3
#define ACT_RUN 4
#define ACT_FLY 5
#define ACT_HOP 7
#define ACT_LEAP 8
#define ACT_COWER 25
#define ACT_SMALL_FLINCH 26
#define ACT_BIG_FLINCH 27
#define ACT_RANGE_ATTACK1 28
#define ACT_RANGE_ATTACK2 29
#define ACT_MELEE_ATTACK1 30
#define ACT_MELEE_ATTACK2 31
#define ACT_DIESIMPLE 36
#define ACT_DIEBACKWARD 37
#define ACT_DIEFORWARD 38
#define ACT_IDLE_ANGRY 47
#define ACT_HOVER 50
#define ACT_COMBAT_IDLE 62
#define ACT_WALK_SCARED 63
#define ACT_RUN_SCARED 64

typedef struct { const char *idle; int ni; const char *alert; int na; const char *pain; int np; const char *die; int nd; const char *atk; int nat; } sndset_t;

static const sndset_t S_ZOMBIE = {"zombie/zo_idle%d.wav", 4, "zombie/zo_alert%d0.wav", 3, "zombie/zo_pain%d.wav", 2, "zombie/zo_pain%d.wav", 2, "zombie/zo_attack%d.wav", 2};
static const sndset_t S_HEADCRAB = {"headcrab/hc_idle%d.wav", 5, "headcrab/hc_alert%d.wav", 1, "headcrab/hc_pain%d.wav", 3, "headcrab/hc_die%d.wav", 2, "headcrab/hc_attack%d.wav", 3};
static const sndset_t S_HOUND = {"houndeye/he_idle%d.wav", 3, "houndeye/he_alert%d.wav", 3, "houndeye/he_pain%d.wav", 5, "houndeye/he_die%d.wav", 3, "houndeye/he_attack%d.wav", 3};
static const sndset_t S_SLAVE = {NULL, 0, "aslave/slv_alert%d.wav", 1, "aslave/slv_pain%d.wav", 2, "aslave/slv_die%d.wav", 2, NULL, 0};
static const sndset_t S_GRUNT = {NULL, 0, NULL, 0, "hgrunt/gr_pain%d.wav", 5, "hgrunt/gr_die%d.wav", 3, NULL, 0};
static const sndset_t S_AGRUNT = {"agrunt/ag_idle%d.wav", 4, "agrunt/ag_alert%d.wav", 5, "agrunt/ag_pain%d.wav", 5, "agrunt/ag_die%d.wav", 5, "agrunt/ag_attack%d.wav", 3};
static const sndset_t S_SQUID = {"bullchicken/bc_idle%d.wav", 5, "bullchicken/bc_idle%d.wav", 2, "bullchicken/bc_pain%d.wav", 4, "bullchicken/bc_die%d.wav", 3, "bullchicken/bc_attack%d.wav", 3};
static const sndset_t S_CONTROLLER = {"controller/con_idle%d.wav", 5, "controller/con_alert%d.wav", 3, "controller/con_pain%d.wav", 3, "controller/con_die%d.wav", 2, "controller/con_attack%d.wav", 3};
static const sndset_t S_GARG = {"garg/gar_idle%d.wav", 5, "garg/gar_alert%d.wav", 3, "garg/gar_pain%d.wav", 3, "garg/gar_die%d.wav", 2, "garg/gar_attack%d.wav", 3};
static const sndset_t S_ICKY = {"ichy/ichy_idle%d.wav", 4, "ichy/ichy_alert%d.wav", 3, "ichy/ichy_pain%d.wav", 3, "ichy/ichy_die%d.wav", 4, "ichy/ichy_attack%d.wav", 2};
static const sndset_t S_SCI = {NULL, 0, NULL, 0, "scientist/sci_pain%d.wav", 10, "scientist/sci_die%d.wav", 4, NULL, 0};
static const sndset_t S_BARNEY = {NULL, 0, NULL, 0, "barney/ba_pain%d.wav", 3, "barney/ba_die%d.wav", 3, NULL, 0};
static const sndset_t S_TURRET = {NULL, 0, "turret/tu_alert.wav", 1, NULL, 0, "turret/tu_die%d.wav", 1, NULL, 0};
static const sndset_t S_BARNACLE = {NULL, 0, "barnacle/bcl_alert2.wav", 1, NULL, 0, "barnacle/bcl_die%d.wav", 1, "barnacle/bcl_chew%d.wav", 3};
static const sndset_t S_LEECH = {NULL, 0, "leech/leech_alert%d.wav", 2, NULL, 0, NULL, 0, "leech/leech_bite%d.wav", 3};
static const sndset_t S_TENTACLE = {"tentacle/te_sing%d.wav", 2, "tentacle/te_alert%d.wav", 2, NULL, 0, "tentacle/te_death2.wav", 1, "tentacle/te_strike%d.wav", 2};

typedef struct {
    const char *cls, *model;
    float mins[3], maxs[3];
    uint16_t flags;
    const char *hp; float hpdef;
    uint8_t klass;
    float melee_range; const char *melee_dmg;
    uint8_t ranged; float rmin, rmax; const char *ranged_dmg; float rcool;
    const sndset_t *snd;
} mdef_t;

// classname -> model, hull (UTIL_SetSize values from the SDK monsters) and behaviour
static const mdef_t defs[] = {
    {"monster_scientist", "scientist", {-16, -16, 0}, {16, 16, 72}, NF_TALKER | NF_SCI, "sk_scientist_health", 20, C_ALLY, 0, NULL, R_NONE, 0, 0, NULL, 0, &S_SCI},
    {"monster_sitting_scientist", "scientist", {-14, -14, 0}, {14, 14, 36}, NF_SITTING | NF_TALKER | NF_SCI | NF_STATIC, "sk_scientist_health", 20, C_ALLY, 0, NULL, R_NONE, 0, 0, NULL, 0, &S_SCI},
    {"monster_scientist_dead", "scientist", {-16, -16, 0}, {16, 16, 72}, NF_DEAD | NF_NOTSOLID, NULL, 0, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"monster_barney", "barney", {-16, -16, 0}, {16, 16, 72}, NF_TALKER, "sk_barney_health", 35, C_ALLY, 0, NULL, R_BULLET, 0, 1024, "sk_9mm_bullet", 0.5f, &S_BARNEY},
    {"monster_barney_dead", "barney", {-16, -16, 0}, {16, 16, 72}, NF_DEAD | NF_NOTSOLID, NULL, 0, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"monster_gman", "gman", {-16, -16, 0}, {16, 16, 72}, NF_NOAI | NF_INVULN, NULL, 100, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"monster_headcrab", "headcrab", {-12, -12, 0}, {12, 12, 24}, 0, "sk_headcrab_health", 10, C_PREY, 0, NULL, R_LEAP, 64, 256, "sk_headcrab_dmg_bite", 2.0f, &S_HEADCRAB},
    {"monster_babycrab", "baby_headcrab", {-12, -12, 0}, {12, 12, 24}, 0, NULL, 3, C_PREY, 0, NULL, R_LEAP, 32, 180, "sk_headcrab_dmg_bite", 2.0f, &S_HEADCRAB},
    {"monster_zombie", "zombie", {-16, -16, 0}, {16, 16, 72}, 0, "sk_zombie_health", 50, C_ALIEN, 70, "sk_zombie_dmg_one_slash", R_NONE, 0, 0, NULL, 0, &S_ZOMBIE},
    {"monster_human_grunt", "hgrunt", {-16, -16, 0}, {16, 16, 72}, 0, "sk_hgrunt_health", 50, C_MILITARY, 64, "sk_hgrunt_kick", R_BULLET, 0, 2048, "sk_9mmAR_bullet", 0.9f, &S_GRUNT},
    {"monster_hgrunt_dead", "hgrunt", {-16, -16, 0}, {16, 16, 72}, NF_DEAD | NF_NOTSOLID, NULL, 0, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"monster_alien_slave", "islave", {-16, -16, 0}, {16, 16, 72}, 0, "sk_islave_health", 30, C_ALIEN, 64, "sk_islave_dmg_claw", R_ZAP, 64, 1024, "sk_islave_dmg_zap", 2.5f, &S_SLAVE},
    {"monster_vortigaunt", "islave", {-16, -16, 0}, {16, 16, 72}, 0, "sk_islave_health", 30, C_ALIEN, 64, "sk_islave_dmg_claw", R_ZAP, 64, 1024, "sk_islave_dmg_zap", 2.5f, &S_SLAVE},
    {"monster_alien_grunt", "agrunt", {-32, -32, 0}, {32, 32, 64}, 0, "sk_agrunt_health", 60, C_ALIEN, 100, "sk_agrunt_dmg_punch", R_HORNET, 100, 1024, "sk_hornet_dmg", 1.2f, &S_AGRUNT},
    {"monster_bullchicken", "bullsquid", {-32, -32, 0}, {32, 32, 64}, 0, "sk_bullsquid_health", 40, C_PREY, 85, "sk_bullsquid_dmg_bite", R_SPIT, 64, 784, "sk_bullsquid_dmg_spit", 3.0f, &S_SQUID},
    {"monster_houndeye", "houndeye", {-16, -16, 0}, {16, 16, 36}, 0, "sk_houndeye_health", 20, C_ALIEN, 0, NULL, R_SONIC, 0, 192, "sk_houndeye_dmg_blast", 2.5f, &S_HOUND},
    {"monster_barnacle", "barnacle", {-16, -16, -32}, {16, 16, 0}, NF_NODROP | NF_STATIC, NULL, 25, C_ALIEN, 0, NULL, R_NONE, 0, 0, NULL, 0, &S_BARNACLE},
    {"monster_leech", "leech", {-1, -1, 0}, {1, 1, 2}, NF_NOTSOLID | NF_NODROP | NF_FLY, "sk_leech_health", 2, C_ALIEN, 32, "sk_leech_dmg_bite", R_NONE, 0, 0, NULL, 0, &S_LEECH},
    {"monster_cockroach", "roach", {-1, -1, 0}, {1, 1, 2}, NF_NOTSOLID, NULL, 1, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"monster_rat", "bigrat", {0, 0, 0}, {4, 4, 4}, NF_NOTSOLID, NULL, 8, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"monster_alien_controller", "controller", {-32, -32, 0}, {32, 32, 64}, NF_NODROP | NF_FLY, "sk_controller_health", 60, C_ALIEN, 0, NULL, R_BALL, 64, 2048, "sk_controller_dmgball", 2.0f, &S_CONTROLLER},
    {"monster_human_assassin", "hassassin", {-16, -16, 0}, {16, 16, 72}, 0, "sk_hassassin_health", 30, C_MILITARY, 0, NULL, R_BULLET, 64, 2048, "sk_9mm_bullet", 0.6f, NULL},
    {"monster_gargantua", "garg", {-32, -32, 0}, {32, 32, 64}, 0, "sk_gargantua_health", 800, C_ALIEN, 140, "sk_gargantua_dmg_slash", R_FLAME, 0, 330, "sk_gargantua_dmg_fire", 0.2f, &S_GARG},
    {"monster_ichthyosaur", "icky", {-32, -32, -32}, {32, 32, 32}, NF_NODROP | NF_FLY, "sk_ichthyosaur_health", 200, C_ALIEN, 100, "sk_ichthyosaur_shake", R_NONE, 0, 0, NULL, 0, &S_ICKY},
    {"monster_sentry", "sentry", {-16, -16, 0}, {16, 16, 48}, NF_STATIC, "sk_sentry_health", 40, C_MACHINE, 0, NULL, R_BULLET, 0, 1200, "sk_9mm_bullet", 0.1f, &S_TURRET},
    {"monster_miniturret", "miniturret", {-16, -16, -16}, {16, 16, 16}, NF_NODROP | NF_STATIC, "sk_miniturret_health", 40, C_MACHINE, 0, NULL, R_BULLET, 0, 1200, "sk_9mm_bullet", 0.1f, &S_TURRET},
    {"monster_turret", "turret", {-32, -32, -32}, {32, 32, 32}, NF_NODROP | NF_STATIC, "sk_turret_health", 50, C_MACHINE, 0, NULL, R_BULLET, 0, 1200, "sk_12mm_bullet", 0.1f, &S_TURRET},
    {"monster_tentacle", "tentacle2", {-32, -32, 0}, {32, 32, 64}, NF_NODROP | NF_STATIC | NF_INVULN, NULL, 75, C_ALIEN, 380, "sk_gargantua_dmg_slash", R_NONE, 0, 0, NULL, 0, &S_TENTACLE},
    {"monster_apache", "apache", {-300, -300, -172}, {300, 300, 8}, NF_NODROP | NF_NOTSOLID | NF_NOAI, "sk_apache_health", 250, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"monster_osprey", "osprey", {-400, -400, -100}, {400, 400, 100}, NF_NODROP | NF_NOTSOLID | NF_NOAI, NULL, 400, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"monster_hevsuit_dead", "player", {-16, -16, 0}, {16, 16, 72}, NF_DEAD | NF_NOTSOLID, NULL, 0, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"monster_bigmomma", "big_mom", {-32, -32, 0}, {32, 32, 64}, NF_NOAI, NULL, 1000, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"monster_nihilanth", "nihilanth", {-32, -32, 0}, {32, 32, 64}, NF_NODROP | NF_NOAI | NF_INVULN, NULL, 800, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"monster_flyer_flock", "boid", {-8, -8, 0}, {8, 8, 8}, NF_NODROP | NF_NOTSOLID | NF_NOAI, NULL, 1, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    // items (items.cpp / weapons.cpp / ammo): touched by the player to pick up
    {"item_suit", "w_suit", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"item_battery", "w_battery", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"item_healthkit", "w_medkit", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"item_longjump", "w_longjump", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"item_antidote", "w_antidote", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"item_security", "w_security", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_crowbar", "w_crowbar", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_9mmhandgun", "w_9mmhandgun", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_glock", "w_9mmhandgun", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_357", "w_357", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_python", "w_357", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_9mmAR", "w_9mmar", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_mp5", "w_9mmar", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_shotgun", "w_shotgun", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_crossbow", "w_crossbow", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_rpg", "w_rpg", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_gauss", "w_gauss", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_egon", "w_egon", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_hornetgun", "w_hgun", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_handgrenade", "w_grenade", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_satchel", "w_satchel", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_tripmine", "v_tripmine", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weapon_snark", "w_sqknest", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"ammo_9mmclip", "w_9mmclip", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"ammo_glockclip", "w_9mmclip", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"ammo_9mmAR", "w_9mmarclip", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"ammo_mp5clip", "w_9mmarclip", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"ammo_9mmbox", "w_chainammo", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"ammo_ARgrenades", "w_argrenade", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"ammo_mp5grenades", "w_argrenade", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"ammo_buckshot", "w_shotbox", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"ammo_357", "w_357ammobox", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"ammo_crossbow", "w_crossbow_clip", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"ammo_rpgclip", "w_rpgammo", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"ammo_gaussclip", "w_gaussammo", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"ammo_egonclip", "w_gaussammo", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
    {"weaponbox", "w_weaponbox", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
};
#define NUM_DEFS ((int)(sizeof(defs) / sizeof(defs[0])))

static const char *dead_poses_sci[] = {"lying_on_back", "lying_on_stomach", "dead_sitting", "dead_hang", "dead_table1", "dead_table2", "dead_table3"};
static const char *dead_poses_barney[] = {"lying_on_back", "lying_on_side", "lying_on_stomach"};
static const char *dead_poses_grunt[] = {"deadstomach", "deadside", "deadsitting"};
static const char *dead_poses_hev[] = {"deadback", "deadsitting", "deadstomach", "deadtable"};

// ---------------------------------------------------------------- helpers
static inline const mdef_t *D(npc_t *n) { return n->def >= 0 ? &defs[n->def] : NULL; }

static float angle_diff(float a, float b)
{
    float d = fmodf(a - b, 360.0f);
    if (d > 180) d -= 360;
    if (d < -180) d += 360;
    return d;
}

static float dist2d(const float *a, const float *b)
{
    float x = a[0] - b[0], y = a[1] - b[1];
    return sqrtf(x * x + y * y);
}

static float dist3(const float *a, const float *b)
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}

static float yaw_to(const float *from, const float *to)
{
    return atan2f(to[1] - from[1], to[0] - from[0]) * 180 / (float)M_PI;
}

static int find_npc_for_entity(int ent)
{
    for (int i = 0; i < num_npcs; i++) if (npcs[i].ent == ent) return i;
    return -1;
}
int npc_for_entity(int ent) { return find_npc_for_entity(ent); }

static bool is_dead(npc_t *n) { return (n->flags & NF_DEAD) || n->ai == AI_DYING || n->ai == AI_DEAD; }
bool npc_alive(int i) { return i >= 0 && i < num_npcs && !is_dead(&npcs[i]) && !(npcs[i].flags & NF_ITEM) && !entities[npcs[i].ent].removed; }
bool npc_is_item(int i) { return npcs[i].flags & NF_ITEM; }
float npc_eye_height(int i) { npc_t *n = &npcs[i]; return n->maxs[2] * 0.9f; }

static void npc_eye(npc_t *n, float *o)
{
    o[0] = n->origin[0]; o[1] = n->origin[1]; o[2] = n->origin[2] + (n->mins[2] + n->maxs[2]) * 0.5f + (n->maxs[2] - n->mins[2]) * 0.35f;
}

static void npc_center(npc_t *n, float *o)
{
    o[0] = n->origin[0]; o[1] = n->origin[1]; o[2] = n->origin[2] + (n->mins[2] + n->maxs[2]) * 0.5f;
}

static void drop_to_floor(float *o)
{
    hull_t *h = &world.models[0].hulls[0];
    float a[3] = {o[0], o[1], o[2] + 1}, b[3] = {o[0], o[1], o[2] - 256};
    btrace_t tr;
    bsp_trace_hull(h, a, b, &tr);
    if (!tr.allsolid && tr.fraction < 1.0f) o[2] = tr.endpos[2];
}

// DROP_TO_FLOOR with the monster's own box, against the world and brush entities (chairs,
// desks): a point trace drops seated or edge-placed characters through their furniture
static void drop_box(npc_t *n, float *o)
{
    float st[3] = {o[0], o[1], o[2] + 1}, end[3] = {o[0], o[1], o[2] - 256};
    gtrace_t tr;
    game_trace(st, end, n->mins, n->maxs, TR_IGNORE_MONSTERS | TR_IGNORE_PLAYER, n - npcs, &tr);
    if (tr.allsolid || tr.startsolid) {
        // placed a little into the floor (the training holograms): look for free space just above
        for (int up = 4; up <= 72; up += 4) {
            float s2[3] = {o[0], o[1], o[2] + up};
            game_trace(s2, end, n->mins, n->maxs, TR_IGNORE_MONSTERS | TR_IGNORE_PLAYER, n - npcs, &tr);
            if (!tr.startsolid && !tr.allsolid) { if (tr.fraction < 1.0f) o[2] = tr.endpos[2]; return; }
        }
        return;     // inside something: leave it as placed
    }
    if (tr.fraction < 1.0f) o[2] = tr.endpos[2];
}

static void set_seq(npc_t *n, int seq)
{
    if (seq < 0 || !n->model || seq >= n->model->numseqs) seq = 0;
    if (n->seq != seq) n->frame = 0;
    n->seq = seq;
    n->last_frame = -1;
    n->seq_done = false;
    n->framerate = 1.0f;
}

static void restart_seq(npc_t *n, int seq)
{
    set_seq(n, seq);
    n->frame = 0;
}

static int act_seq(npc_t *n, int act)
{
    if (!n->model) return -1;
    int c = 0, pick = -1;
    for (int i = 0; i < n->model->numseqs; i++)
        if (n->model->seqs[i].activity == act) { c++; if (rand() % c == 0) pick = i; }
    return pick;
}

// switch to an activity (keeps playing if it already is)
static bool set_activity(npc_t *n, int act)
{
    if (n->seq >= 0 && n->model && n->model->seqs[n->seq].activity == act) return true;
    int s = act_seq(n, act);
    if (s < 0) return false;
    set_seq(n, s);
    return true;
}

static void model_name_from_path(const char *path, char *out, int n)
{
    const char *s = strrchr(path, '/');
    s = s ? s + 1 : path;
    int i = 0;
    while (s[i] && s[i] != '.' && i < n - 1) { out[i] = tolower((unsigned char)s[i]); i++; }
    out[i] = 0;
}

static void npc_sound(npc_t *n, int chan, const char *fmt, int count, float vol, float attn)
{
    if (!fmt || count <= 0) return;
    char s[64];
    snprintf(s, sizeof(s), fmt, 1 + rand() % count);
    snd_emit(n->ent, chan, s, vol, attn, n->pitch > 0 ? n->pitch : PITCH_NORM, NULL);
}

static int head_pitch(npc_t *n)
{
    // CScientist::TalkInit: voice pitch by head
    if (!(n->flags & NF_SCI)) return 100;
    switch (n->body) { case 0: return 105; case 1: return 100; case 2: return 95; default: return 100; }
}

// ---------------------------------------------------------------- spawning
static int def_for_class(const char *cls)
{
    for (int k = 0; k < NUM_DEFS; k++) if (!strcasecmp(cls, defs[k].cls)) return k;
    return -1;
}

static npc_t *new_npc(int ent, int def, studio_t *m)
{
    if (num_npcs >= MAX_NPCS) return NULL;
    npc_t *n = &npcs[num_npcs++];
    memset(n, 0, sizeof(*n));
    n->ent = ent;
    n->def = def;
    n->model = m;
    n->script = -1;
    n->seq = -1;
    n->enemy = -1;
    n->talk_target = -1;
    n->trig_cond = ent_value(&entities[ent], "TriggerTarget") && ent_value(&entities[ent], "TriggerCondition") ? atoi(ent_value(&entities[ent], "TriggerCondition")) : -1;
    n->voice = -1;
    n->onground = true;
    n->ground_ent = -1;
    return n;
}

static void spawn_one(int i, const char *cls_override)
{
    entity_t *e = &entities[i];
    const char *cls = cls_override ? cls_override : e->classname;
    int di = def_for_class(cls);
    char mname[32] = "";
    if (di >= 0) {
        strcpy(mname, defs[di].model);
    } else if (!strcasecmp(cls, "monster_generic") || !strcasecmp(cls, "cycler") ||
               !strcasecmp(cls, "monster_furniture") || !strcasecmp(cls, "env_model")) {
        const char *mp = ent_value(e, "model");
        if (!mp || strncasecmp(mp, "models/", 7)) return;
        model_name_from_path(mp, mname, sizeof(mname));
    } else {
        return;
    }
    studio_t *m = studio_get(mname);
    if (!m) return;
    npc_t *n = new_npc(i, di, m);
    if (!n) return;
    const mdef_t *d = di >= 0 ? &defs[di] : NULL;
    memcpy(n->origin, e->origin, 12);
    n->yaw = e->angles[1];
    n->flags = d ? d->flags : NF_NOAI;
    if (d) { memcpy(n->mins, d->mins, 12); memcpy(n->maxs, d->maxs, 12); }
    else { float a[3] = {-16, -16, 0}, b[3] = {16, 16, 72}; memcpy(n->mins, a, 12); memcpy(n->maxs, b, 12); }
    if (!d && !strcasecmp(cls, "monster_furniture")) n->flags |= NF_INVULN;
    if (e->spawnflags & 256) n->flags |= NF_PREDIS;
    if (e->spawnflags & 2) n->flags |= NF_GAG;
    if (e->spawnflags & 16) n->flags |= NF_PRISONER;
    if (e->spawnflags & 1) n->flags |= NF_WAITSEEN;
    n->solid = !(n->flags & (NF_NOTSOLID | NF_ITEM));
    if (!strcasecmp(cls, "monster_generic") && (e->spawnflags & 4)) n->solid = false;   // SF_GENERICMONSTER_NOTSOLID
    // body / skin (scientist heads are picked at random like CScientist::Spawn)
    const char *v = ent_value(e, "body");
    n->body = v ? atoi(v) : 0;
    v = ent_value(e, "skin");
    n->skin = v ? atoi(v) : 0;
    if (!strcmp(mname, "scientist") && n->body == -1) n->body = rand() % 4;
    if (n->body < 0) n->body = 0;
    if (!strcmp(mname, "scientist") && n->body == 2) n->skin = 1;   // HEAD_LUTHER
    if (!strcmp(mname, "hgrunt") && !ent_value(e, "body")) n->body = 0;
    n->pitch = head_pitch(n);
    // cyclers, env_models and furniture keep their placement (MOVETYPE_NONE); monsters drop
    // holograms (renderfx 16, the training-room Gina) float where the mapper put them, on
    // emitters that aren't solid to monsters: dropping them sank her into the pedestal
    {
        const char *fx = ent_value(e, "renderfx");
        if (fx && atoi(fx) == 16) n->flags |= NF_NODROP;
    }
    bool nodrop = (n->flags & NF_NODROP) || (!d && strcasecmp(cls, "monster_generic"));
    if (!nodrop) drop_box(n, n->origin);
    n->health = n->maxhealth = d ? (d->hp ? skill_get(d->hp) : d->hpdef) : 100;
    if (n->health <= 0) n->health = n->maxhealth = d && d->hpdef > 0 ? d->hpdef : 20;
    if (e->health > 0 && !(n->flags & NF_ITEM)) n->health = n->maxhealth = e->health;

    // starting animation
    int seq = -1;
    if (n->flags & NF_DEAD) {
        int pose = (int)atof(ent_value(e, "pose") ? ent_value(e, "pose") : "0");
        const char **tab = dead_poses_sci; int cnt = 7;
        if (!strcmp(mname, "barney")) { tab = dead_poses_barney; cnt = 3; }
        else if (!strcmp(mname, "hgrunt")) { tab = dead_poses_grunt; cnt = 3; }
        else if (!strcmp(mname, "player")) { tab = dead_poses_hev; cnt = 4; }
        if (pose < 0 || pose >= cnt) pose = 0;
        seq = studio_find_seq(m, tab[pose]);
        n->ai = AI_DEAD;
    } else if (n->flags & NF_SITTING) {
        seq = studio_find_seq(m, "sitting2");
        if (seq < 0) seq = studio_find_seq(m, "sitlookleft");
    } else if ((v = ent_value(e, "sequence"))) {
        // sequence numbers refer to the full .mdl; the converter stores each kept one's original index
        int want = atoi(v);
        for (int k = 0; k < m->numseqs; k++) if (m->seqs[k].pad == want) { seq = k; break; }
    }
    if (seq < 0) seq = studio_find_activity(m, ACT_IDLE);
    if (seq < 0) seq = 0;
    n->idle_seq = seq;
    set_seq(n, seq);
    n->frame = (rand() % 100) / 100.0f * (m->seqs[seq].numframes - 1);  // desync idles
    n->next_look = 0.5f + (rand() % 100) / 100.0f;
    n->idle_speak_time = 5 + rand() % 10;
    bsp_light_at(n->origin, n->light);
}

// the animations this map needs (studio.c drops the others at load time to save RAM)
static void set_anim_filter(void)
{
    static char names[160][24];
    static const char *ptrs[160];
    static const int acts[] = {ACT_IDLE, ACT_WALK, ACT_RUN, ACT_FLY, ACT_LEAP, ACT_COWER, ACT_SMALL_FLINCH, ACT_RANGE_ATTACK1,
                               ACT_RANGE_ATTACK2, ACT_MELEE_ATTACK1, ACT_MELEE_ATTACK2, ACT_DIESIMPLE, ACT_DIEBACKWARD,
                               ACT_COMBAT_IDLE, ACT_RUN_SCARED};
    static const char *fixed[] = {"lying_on_back", "lying_on_stomach", "lying_on_side", "dead_sitting", "dead_hang",
                                  "dead_table1", "dead_table2", "dead_table3", "deadstomach", "deadside", "deadsitting",
                                  "deadback", "deadtable", "sitting2", "sitlookleft"};
    int n = 0;
    for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) { strcpy(names[n], fixed[i]); n++; }
    for (int i = 1; i < num_entities && n < 158; i++) {
        entity_t *e = &entities[i];
        const char *v;
        if (e->type == ENT_SCRIPTED) {
            if ((v = ent_value(e, "m_iszPlay"))) { strncpy(names[n], v, 23); names[n][23] = 0; n++; }
            if ((v = ent_value(e, "m_iszIdle"))) { strncpy(names[n], v, 23); names[n][23] = 0; n++; }
        } else if (!strncasecmp(e->classname, "monster_", 8) && (v = ent_value(e, "sequence"))) {
            snprintf(names[n], 24, "#%d", atoi(v)); n++;
        }
    }
    for (int i = 0; i < n; i++) ptrs[i] = names[i];
    studio_set_filter(ptrs, n, acts, sizeof(acts) / sizeof(acts[0]));
}

void npcs_spawn_all(void)
{
    num_npcs = 0;
    talk_wait_time = 0;
    set_anim_filter();
    game_trace_begin_frame();
    for (int i = 1; i < num_entities && num_npcs < MAX_NPCS; i++) {
        entity_t *e = &entities[i];
        if (!strcasecmp(e->classname, "world_items")) {
            // items.cpp CWorldItem: type 42 antidote, 43 security, 44 battery, 45 suit
            static const char *wi[] = {"item_antidote", "item_security", "item_battery", "item_suit"};
            int t = (int)atof(ent_value(e, "type") ? ent_value(e, "type") : "0");
            if (t >= 42 && t <= 45) spawn_one(i, wi[t - 42]);
            continue;
        }
        spawn_one(i, NULL);
    }
    debugf("npcs: %d spawned\n", num_npcs);
}

// an item created during play (from a crate, or dropped by a dead soldier)
int npc_spawn_item(const char *classname, const float *origin, float yaw)
{
    if (num_entities >= MAX_ENTITIES) return -1;
    static char kvpool[64][96];
    static int kvnext;
    char *kv = kvpool[kvnext++ & 63];
    snprintf(kv, 96, "\"classname\" \"%s\" }", classname);
    entity_t *e = &entities[num_entities];
    memset(e, 0, sizeof(*e));
    e->kv = kv;
    strncpy(e->classname, classname, sizeof(e->classname) - 1);
    e->type = ENT_OTHER;
    e->model = -1;
    e->renderamt = 255;
    memcpy(e->origin, origin, 12);
    e->angles[1] = yaw;
    int idx = num_entities++;
    int before = num_npcs;
    spawn_one(idx, NULL);
    return num_npcs > before ? num_npcs - 1 : -1;
}

// ---------------------------------------------------------------- movement
static bool box_free(npc_t *n, const float *p, int self)
{
    gtrace_t tr;
    game_trace(p, p, n->mins, n->maxs, 0, self, &tr);
    return !tr.startsolid;
}

// SV_movestep: step up to 18 units, never off a ledge (unless `allow_fall`); slides along walls
static bool walk_step(npc_t *n, int self, float dx, float dy, bool allow_fall, int *blocker)
{
    float up[3] = {n->origin[0], n->origin[1], n->origin[2] + 18};
    gtrace_t t0;
    game_trace(up, up, n->mins, n->maxs, 0, self, &t0);
    if (t0.startsolid) up[2] = n->origin[2];
    float fw[3] = {up[0] + dx, up[1] + dy, up[2]};
    gtrace_t t1;
    game_trace(up, fw, n->mins, n->maxs, 0, self, &t1);
    if (t1.startsolid) return false;
    if (t1.fraction < 1 && blocker) *blocker = t1.ent;
    if (t1.fraction < 0.05f) {
        // slide: try each axis on its own
        float ax[3] = {up[0] + dx, up[1], up[2]}, ay[3] = {up[0], up[1] + dy, up[2]};
        gtrace_t tx, ty;
        game_trace(up, ax, n->mins, n->maxs, 0, self, &tx);
        game_trace(up, ay, n->mins, n->maxs, 0, self, &ty);
        if (fabsf(dx) * tx.fraction >= fabsf(dy) * ty.fraction) t1 = tx; else t1 = ty;
        if (t1.startsolid || t1.fraction < 0.05f) return false;
    }
    float dn[3] = {t1.endpos[0], t1.endpos[1], t1.endpos[2] - 18 - 18};
    gtrace_t t2;
    game_trace(t1.endpos, dn, n->mins, n->maxs, 0, self, &t2);
    if (t2.startsolid) return false;
    if (t2.fraction >= 1) {
        if (!allow_fall) return false;      // a ledge: MOVE_STEP won't walk off it
        memcpy(n->origin, dn, 12);
        n->onground = false;
        n->moved = true;
        return true;
    }
    memcpy(n->origin, t2.endpos, 12);
    n->onground = true;
    n->ground_ent = t2.ent;
    n->moved = true;
    return true;
}

static int npc_hull(npc_t *n)
{
    float sx = n->maxs[0] - n->mins[0], sz = n->maxs[2] - n->mins[2];
    if (sx <= 36) return sz <= 36 ? 3 : 1;
    return 2;
}

// can the monster walk straight from a to b (CheckLocalMove)
typedef struct { npc_t *n; int self; } walkctx_t;
static bool local_walk(const float *a, const float *b, void *vctx)
{
    walkctx_t *c = vctx;
    npc_t *n = c->n;
    if (n->flags & NF_FLY) {
        gtrace_t tr;
        game_trace(a, b, n->mins, n->maxs, TR_IGNORE_PLAYER | TR_IGNORE_MONSTERS, c->self, &tr);
        return tr.fraction >= 1 && !tr.startsolid;
    }
    float save[3];
    memcpy(save, n->origin, 12);
    bool save_g = n->onground;
    memcpy(n->origin, a, 12);
    float d = dist2d(a, b);
    bool ok = true;
    float dx = (b[0] - a[0]) / (d > 0 ? d : 1), dy = (b[1] - a[1]) / (d > 0 ? d : 1);
    for (float moved = 0; moved < d - 1 && ok; moved += 24) {
        float st = d - moved > 24 ? 24 : d - moved;
        float before[3];
        memcpy(before, n->origin, 12);
        if (!walk_step(n, c->self, dx * st, dy * st, false, NULL)) ok = false;
        else if (dist2d(before, n->origin) < st * 0.5f) ok = false;   // slid instead of advancing
    }
    if (ok && fabsf(n->origin[2] - b[2]) > 40) ok = false;
    memcpy(n->origin, save, 12);
    n->onground = save_g;
    return ok;
}

static void route_to(npc_t *n, const float *goal)
{
    int self = n - npcs;
    walkctx_t c = {n, self};
    memcpy(n->goal, goal, 12);
    n->pathi = 0;
    n->stuck_time = 0;
    if (local_walk(n->origin, goal, &c)) {
        memcpy(n->path[0], goal, 12);
        n->npath = 1;
        return;
    }
    int hull = npc_hull(n);
    bool air = (n->flags & NF_FLY) != 0;
    int a = nav_nearest(n->origin, hull, air, local_walk, &c);
    // the goal end: a node from which the goal can be walked to
    int b = -1;
    if (a >= 0) {
        float save[3];
        memcpy(save, n->origin, 12);
        b = nav_nearest(goal, hull, air, NULL, NULL);
        // prefer a node the goal is directly reachable from
        int cand = nav_nearest(goal, hull, air, local_walk, &c);
        if (cand >= 0) b = cand;
        memcpy(n->origin, save, 12);
    }
    int route[MAX_PATH_PTS];
    int len = (a >= 0 && b >= 0) ? nav_route(a, b, hull, route, MAX_PATH_PTS - 1) : 0;
    if (len <= 0) {
        debugf("route %s: none (%d->%d) from %.0f %.0f %.0f to %.0f %.0f %.0f\n", entities[n->ent].targetname, a, b, n->origin[0], n->origin[1], n->origin[2], goal[0], goal[1], goal[2]);
        // no route: head straight for it (the movement code stops at walls)
        memcpy(n->path[0], goal, 12);
        n->npath = 1;
        return;
    }
    n->npath = 0;
    debugf("route %s: %d nodes (%d->%d)\n", entities[n->ent].targetname, len, a, b);
    for (int i = 0; i < len; i++) memcpy(n->path[n->npath++], nav_node_origin(route[i]), 12);
    memcpy(n->path[n->npath++], goal, 12);
    // skip the first node when the second is already reachable (route simplification)
    if (n->npath > 2 && local_walk(n->origin, n->path[1], &c)) n->pathi = 1;
}

// move along the current route; returns true on arrival
static bool follow_route(npc_t *n, float speed, float dt, float arrive_dist)
{
    int self = n - npcs;
    if (n->pathi >= n->npath) return true;
    float *wp = n->path[n->pathi];
    bool last = n->pathi == n->npath - 1;
    float d = (n->flags & NF_FLY) ? dist3(n->origin, wp) : dist2d(n->origin, wp);
    if (d <= (last ? arrive_dist : 16)) {
        n->pathi++;
        if (n->pathi >= n->npath) return true;
        // look ahead: cut the corner when the following point is reachable
        if (n->pathi < n->npath - 1) {
            walkctx_t c = {n, self};
            if (local_walk(n->origin, n->path[n->pathi + 1], &c)) n->pathi++;
        }
        wp = n->path[n->pathi];
        d = dist2d(n->origin, wp);
    }
    float want = yaw_to(n->origin, wp);
    float dy = angle_diff(want, n->yaw);
    float mt = (n->running ? 300 : 200) * dt;
    n->yaw += dy > mt ? mt : (dy < -mt ? -mt : dy);
    float step = speed * dt;
    if (step > d) step = d;
    if (fabsf(dy) > 60) step *= 0.3f;   // turn on the spot first
    float a = n->yaw * (float)M_PI / 180;
    if (n->flags & NF_FLY) {
        float dir[3] = {wp[0] - n->origin[0], wp[1] - n->origin[1], wp[2] - n->origin[2]};
        float l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (l > 0.1f) {
            float to[3] = {n->origin[0] + dir[0] / l * step, n->origin[1] + dir[1] / l * step, n->origin[2] + dir[2] / l * step};
            gtrace_t tr;
            game_trace(n->origin, to, n->mins, n->maxs, 0, self, &tr);
            if (!tr.startsolid) memcpy(n->origin, tr.endpos, 12);
        }
    } else {
        int blocker = -1;
        walk_step(n, self, cosf(a) * step, sinf(a) * step, false, &blocker);
        // walking into a door that opens by touch (CAP_OPEN_DOORS)
        if (blocker > 0 && blocker < num_entities) {
            entity_t *be = &entities[blocker];
            if ((be->type == ENT_DOOR || be->type == ENT_DOOR_ROTATING) && !be->targetname[0] &&
                (be->state == MS_AT_BOTTOM) && (D(n) && (D(n)->klass == C_ALLY || D(n)->klass == C_MILITARY)))
                ents_use_entity(blocker);
        }
    }
    // stuck detection
    n->moved_check += dt;
    if (n->moved_check >= 0.5f) {
        float moved = dist3(n->origin, n->last_pos);
        if (moved < speed * 0.5f * 0.25f) n->stuck_time += n->moved_check;
        else n->stuck_time = 0;
        memcpy(n->last_pos, n->origin, 12);
        n->moved_check = 0;
    }
    return false;
}

static float move_speed(npc_t *n, bool run)
{
    int ms = act_seq(n, run ? ACT_RUN : ACT_WALK);
    if (ms < 0) ms = act_seq(n, run ? ACT_WALK : ACT_RUN);
    if (ms < 0 && (n->flags & NF_FLY)) ms = act_seq(n, ACT_FLY);
    if (ms >= 0) {
        sseq_t *s = &n->model->seqs[ms];
        float dur = studio_seq_duration(n->model, ms);
        float dist = sqrtf(s->linmove[0] * s->linmove[0] + s->linmove[1] * s->linmove[1]);
        if (dur > 0 && dist > 1) return dist / dur;
    }
    return run ? 200 : 80;
}

static void start_moving(npc_t *n, const float *goal, bool run)
{
    n->running = run;
    n->speed = move_speed(n, run);
    if (!set_activity(n, run ? ACT_RUN : ACT_WALK)) set_activity(n, run ? ACT_WALK : ACT_RUN);
    if (n->flags & NF_FLY) set_activity(n, ACT_FLY);
    route_to(n, goal);
    memcpy(n->last_pos, n->origin, 12);
    n->moved_check = 0;
    n->stuck_count = 0;
}

static void gravity(npc_t *n, float dt)
{
    if ((n->flags & (NF_NODROP | NF_FLY | NF_ITEM)) || n->sstate == NS_SCRIPT_IDLE) return;
    // teleported into a scripted pose (sitting in a chair, leaning on a rail): stay exactly there
    if (n->script > 0 && n->script < num_entities) {
        const char *mv = ent_value(&entities[n->script], "m_fMoveTo");
        if (!mv || atoi(mv) == 4 || atoi(mv) == 0) return;
    }
    int self = n - npcs;
    // standing still on something that isn't moving: nothing to do
    bool moved = n->moved;
    n->moved = false;
    if (n->onground && !moved) {
        if (n->ground_ent <= 0 || n->ground_ent >= num_entities) return;
        entity_t *g = &entities[n->ground_ent];
        if (g->origin[0] == n->ground_prev[0] && g->origin[1] == n->ground_prev[1] && g->origin[2] == n->ground_prev[2]) return;
    }
    // ride whatever we stand on (lifts, trains)
    if (n->ground_ent > 0 && n->ground_ent < num_entities) {
        entity_t *g = &entities[n->ground_ent];
        float d[3] = {g->origin[0] - n->ground_prev[0], g->origin[1] - n->ground_prev[1], g->origin[2] - n->ground_prev[2]};
        if (fabsf(d[0]) + fabsf(d[1]) + fabsf(d[2]) > 0.001f && fabsf(d[0]) + fabsf(d[1]) + fabsf(d[2]) < 64) {
            float to[3] = {n->origin[0] + d[0], n->origin[1] + d[1], n->origin[2] + d[2]};
            if (d[2] > 0) n->origin[2] += d[2], to[2] = n->origin[2];
            gtrace_t tr;
            game_trace(n->origin, to, n->mins, n->maxs, TR_IGNORE_PLAYER, self, &tr);
            if (!tr.startsolid) memcpy(n->origin, tr.endpos, 12);
        }
    }
    // ground check
    float dn[3] = {n->origin[0], n->origin[1], n->origin[2] - (n->onground ? 2 : n->vz * dt + 1)};
    if (!n->onground) { n->vz += 800 * dt; dn[2] = n->origin[2] - n->vz * dt; }
    gtrace_t tr;
    game_trace(n->origin, dn, n->mins, n->maxs, TR_IGNORE_PLAYER, self, &tr);
    if (tr.startsolid) { n->onground = true; n->vz = 0; }
    else if (tr.fraction < 1) {
        memcpy(n->origin, tr.endpos, 12);
        n->onground = true;
        n->vz = 0;
        n->ground_ent = tr.ent;
    } else {
        memcpy(n->origin, tr.endpos, 12);
        n->onground = false;
        n->ground_ent = -1;
    }
    if (n->ground_ent > 0 && n->ground_ent < num_entities) memcpy(n->ground_prev, entities[n->ground_ent].origin, 12);
}

// ---------------------------------------------------------------- speech (talkmonster.cpp)
static bool player_in_pvs(npc_t *n);

static bool ok_to_speak(npc_t *n)
{
    if (is_dead(n) || (n->flags & NF_GAG)) return false;
    if (game_time() <= talk_wait_time) return false;
    if (!player_in_pvs(n)) return false;
    if (n->enemy >= 0 && n->ai == AI_COMBAT) return false;
    return true;
}

bool npc_can_speak(int i, bool override, bool followers_only)
{
    npc_t *n = &npcs[i];
    if (is_dead(n) || entities[n->ent].removed) return false;
    if (followers_only && !n->following) return false;
    if (!(n->flags & NF_TALKER)) return true;     // CBaseMonster::CanPlaySentence: alive
    if (override) return true;
    return ok_to_speak(n);
}

// CTalkMonster::PlaySentence
static void npc_say(npc_t *n, const char *sentence, float duration, float vol, float attn)
{
    if (!sentence || !sentence[0]) return;
    float now = game_time();
    n->talk_time = now + duration;
    talk_wait_time = now + duration + 2.0f;
    n->voice = snd_sentence(n->ent, sentence, vol, attn, n->pitch);
    n->said |= 1;   // bit_saidHelloPlayer: having said anything, don't greet
}

void npc_play_scripted_sentence(int i, const char *sentence, float duration, float vol, float attn, bool concurrent,
                                int listener_ent)
{
    npc_t *n = &npcs[i];
    n->use_time = game_time() + duration;
    if (n->flags & NF_TALKER) {
        npc_say(n, sentence, duration, vol, attn);
        if (concurrent) {}   // (bConcurrent only affects the AI's talk timing)
    } else {
        n->voice = snd_sentence(n->ent, sentence, vol, attn, n->pitch);
    }
    n->talk_target = listener_ent;
}

static const char *grp(npc_t *n, const char *sci, const char *ba)
{
    return (n->flags & NF_SCI) ? sci : ba;
}

// ---------------------------------------------------------------- scripted sequences
static void ai_trigger(npc_t *n, int cond);
static bool can_play_sequence(npc_t *n, bool override_state)
{
    if (n->script >= 0 || is_dead(n) || n->ai == AI_GRABBED) return false;
    if (override_state) return true;
    return n->ai == AI_IDLE || n->ai == AI_COMBAT;
}

// CCineMonster::FindEntity
static int script_find_target(entity_t *sc)
{
    char want[48];
    const char *v = ent_value(sc, "m_iszEntity");
    if (!v) return -1;
    strncpy(want, v, sizeof(want) - 1); want[47] = 0;
    bool override = (sc->spawnflags & 64) != 0;   // SF_SCRIPT_OVERRIDESTATE
    for (int i = 0; i < num_npcs; i++) {
        npc_t *n = &npcs[i];
        entity_t *e = &entities[n->ent];
        if (e->removed || (n->flags & NF_ITEM) || !e->targetname[0] || strcasecmp(e->targetname, want)) continue;
        if (can_play_sequence(n, override)) return i;
    }
    float radius = 0;
    if ((v = ent_value(sc, "m_flRadius"))) radius = atof(v);
    int best = -1;
    float bestd = 1e30f;
    for (int i = 0; i < num_npcs; i++) {
        npc_t *n = &npcs[i];
        entity_t *e = &entities[n->ent];
        if (e->removed || strcasecmp(e->classname, want)) continue;
        float d = dist3(n->origin, sc->origin);
        if (d > radius || !can_play_sequence(n, override)) continue;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

static void stop_following(npc_t *n, bool speak);

// CCineMonster::PossessEntity + the monster's script schedule
static void script_start(int ni, int sci, bool idle_only)
{
    npc_t *n = &npcs[ni];
    entity_t *sc = &entities[sci];
    if (n->following) stop_following(n, false);
    n->script = sci;
    n->said |= 1;   // scripted actors don't greet the player (bit_saidHelloPlayer)
    n->script_idle_only = idle_only;
    n->ai = AI_IDLE;
    n->enemy = -1;
    int moveto = (int)atof(ent_value(sc, "m_fMoveTo") ? ent_value(sc, "m_fMoveTo") : "0");
    memcpy(n->dest, sc->origin, 12);
    if ((moveto == 1 || moveto == 2) && !(n->flags & (NF_NODROP | NF_FLY))) drop_box(n, n->dest);
    n->said |= 1;   // a scripted actor doesn't greet the player afterwards
    n->destyaw = sc->angles[1];
    n->stuck_count = 0;
    // with an idle animation, the monster takes it while it waits (and at the mark, see below)
    if (moveto == 1 || moveto == 2) {
        if (dist2d(n->origin, n->dest) > 4 || fabsf(n->origin[2] - n->dest[2]) > 24) {
            start_moving(n, n->dest, moveto == 2);
            n->sstate = NS_MOVING;
            return;
        }
        n->sstate = NS_TURNING;
    } else {
        if (moveto == 4) {
            memcpy(n->origin, n->dest, 12);
            n->yaw = n->destyaw;
        }
        n->sstate = NS_TURNING;
        if (moveto == 0) n->destyaw = n->yaw;   // play in place
    }
}

static void script_done(npc_t *n)
{
    int sci = n->script;
    n->script = -1;
    n->sstate = NS_IDLE;
    if (sci >= 0 && (entities[sci].spawnflags & 8)) {
        // SF_SCRIPT_LEAVECORPSE: the monster stays as a corpse in the script's last frame
        // (the guards cut in half by the laser in c1a1)
        n->flags |= NF_DEAD | NF_NOTSOLID;
        n->solid = false;
        n->ai = AI_DEAD;
        n->health = 0;
        n->frame = n->model->seqs[n->seq].numframes - 1;
        ents_fire_entity_target(&entities[sci]);
        return;
    }
    set_seq(n, n->idle_seq);
    // CCineMonster::SequenceDone: a repeatable script can be used again; otherwise it's spent
    if (sci >= 0) ents_fire_entity_target(&entities[sci]);
}

static void script_play(npc_t *n)
{
    entity_t *sc = &entities[n->script];
    const char *name = n->script_idle_only ? ent_value(sc, "m_iszIdle") : ent_value(sc, "m_iszPlay");
    if (!name) {
        if (n->script_idle_only) { n->sstate = NS_SCRIPT_IDLE; set_seq(n, n->idle_seq); return; }
        // a start-now script with only an idle pose (Barney at the front desk): the monster
        // keeps that pose while it waits for the next script
        const char *idle = ent_value(sc, "m_iszIdle");
        if (idle) {
            int is = studio_find_seq(n->model, idle);
            if (is >= 0) n->idle_seq = is;
        }
        script_done(n);   // StartSequence(completeOnEmpty): done right away
        return;
    }
    char nm[48];
    strncpy(nm, name, 47); nm[47] = 0;
    int s = studio_find_seq(n->model, nm);
    if (s < 0) { debugf("npc: %s has no sequence %s\n", n->model->name, nm); s = 0; }   // as the SDK: sequence 0
    restart_seq(n, s);
    n->sstate = n->script_idle_only ? NS_SCRIPT_IDLE : NS_PLAYING;
}

// a scripted_sequence was triggered (CCineMonster::Use)
void npc_script_use(int sci)
{
    for (int i = 0; i < num_npcs; i++) {
        if (npcs[i].script == sci) {
            if (npcs[i].sstate == NS_PLAYING || npcs[i].sstate == NS_MOVING) return;   // SCRIPT_PLAYING
            npcs[i].script_idle_only = false;
            if (npcs[i].sstate == NS_SCRIPT_IDLE || npcs[i].sstate == NS_SCRIPT_WAIT) script_start(i, sci, false);
            return;
        }
    }
    int ni = script_find_target(&entities[sci]);
    if (ni < 0) {
        // the monster is busy (or not here yet): CineThink tries again every second
        entities[sci].ltime2 = game_time() + 1.0f;
        entities[sci].charge_on = 1;   // pending
        return;
    }
    entities[sci].charge_on = 0;
    script_start(ni, sci, false);
}

// pending scripts look for their monster again (CCineMonster::CineThink)
static void scripts_retry(void)
{
    float now = game_time();
    for (int i = 1; i < num_entities; i++) {
        entity_t *sc = &entities[i];
        if (sc->type != ENT_SCRIPTED || !sc->charge_on || sc->removed || now < sc->ltime2) continue;
        int ni = script_find_target(sc);
        if (ni < 0) { sc->ltime2 = now + 1.0f; continue; }
        sc->charge_on = 0;
        script_start(ni, i, false);
    }
}

// at map start: scripts without a targetname start by themselves, scripts with an idle
// animation grab their monster and wait (CCineMonster::Spawn)
void npc_scripts_init(void)
{
    for (int i = 1; i < num_entities; i++) {
        entity_t *sc = &entities[i];
        if (sc->type != ENT_SCRIPTED) continue;
        sc->charge_on = 0;
        bool idle = ent_value(sc, "m_iszIdle") != NULL;
        if (sc->targetname[0] && !idle) continue;
        int ni = script_find_target(sc);
        if (ni < 0) { if (!sc->targetname[0]) { sc->charge_on = 1; sc->ltime2 = 1.0f; } continue; }
        // no action animation, only an idle (Barney at the lobby desk): hold the idle pose until triggered
        bool hold = sc->targetname[0] != 0 || !ent_value(sc, "m_iszPlay");
        script_start(ni, i, hold);
    }
}

static void fire_anim_events(npc_t *n, int from, int to)
{
    sseq_t *s = &n->model->seqs[n->seq];
    for (int i = 0; i < s->numevents; i++) {
        sevent_t *ev = (sevent_t *)((uint8_t *)n->model->events + s->eventofs) + i;
        if (ev->frame <= from || ev->frame > to) continue;
        char opt[60];
        strncpy(opt, ev->options, 59); opt[59] = 0;
        switch (ev->event) {
        case 1003: ents_fire_targets(opt); break;                                         // SCRIPT_EVENT_FIREEVENT
        case 1004: snd_emit(n->ent, CHAN_BODY, opt, 1.0f, ATTN_NORM, PITCH_NORM, NULL); break; // SCRIPT_EVENT_SOUND
        case 1008: snd_emit(n->ent, CHAN_VOICE, opt, 1.0f, ATTN_NORM, n->pitch, NULL); break;  // SOUND_VOICE
        case 1005: n->voice = snd_sentence(n->ent, opt, 1.0f, ATTN_IDLE, n->pitch); break;      // SENTENCE
        case 1009: if (rand() % 3 == 0) n->voice = snd_sentence(n->ent, opt, 1.0f, ATTN_IDLE, n->pitch); break;
        default: break;
        }
    }
}

// ---------------------------------------------------------------- perception
static uint8_t pvs_buf[1024];
static bool pvs_ok;
static float player_pos[3], player_eye[3];

static bool player_in_pvs(npc_t *n)
{
    if (!pvs_ok) return true;
    float c[3];
    npc_center(n, c);
    int l = bsp_leaf_for_point(c);
    if (l <= 0) return true;
    return pvs_buf[(l - 1) >> 3] & (1 << ((l - 1) & 7));
}

static int klass(npc_t *n) { const mdef_t *d = D(n); return d ? d->klass : C_NONE; }

// relationship table (monsters.cpp IRelationship, simplified): true = attack
static bool hates(npc_t *n, int target)
{
    if (n->flags & NF_PRISONER) return false;
    int k = klass(n);
    if (target == 0) {
        if (!player_alive() || pm_noclip_active()) return false;
        if (k == C_ALLY) return n->provoked;
        return k == C_MILITARY || k == C_ALIEN || k == C_PREY || k == C_MACHINE;
    }
    npc_t *t = &npcs[target - 1];
    if (!npc_alive(target - 1) || (t->flags & NF_PRISONER)) return false;
    int tk = klass(t);
    switch (k) {
    case C_ALLY: return tk == C_MILITARY || tk == C_ALIEN || tk == C_PREY || tk == C_MACHINE;
    case C_MILITARY: return tk == C_ALLY || tk == C_ALIEN || tk == C_PREY;
    case C_ALIEN: return tk == C_ALLY || tk == C_MILITARY;
    case C_PREY: return tk == C_ALLY || tk == C_MILITARY;
    case C_MACHINE: return tk == C_ALLY || tk == C_ALIEN || tk == C_PREY;
    }
    return false;
}

static void target_pos(int target, float *o)
{
    if (target == 0) memcpy(o, player_pos, 12);
    else { npc_t *t = &npcs[target - 1]; npc_center(t, o); }
}

static bool can_see(npc_t *n, int target, bool need_fov)
{
    float eye[3], tp[3];
    npc_eye(n, eye);
    if (target == 0) memcpy(tp, player_eye, 12);
    else { npc_t *t = &npcs[target - 1]; npc_eye(t, tp); }
    float d = dist3(eye, tp);
    if (d > 2048) return false;
    if (need_fov) {
        float a = n->yaw * (float)M_PI / 180;
        float dx = tp[0] - eye[0], dy = tp[1] - eye[1];
        float l = sqrtf(dx * dx + dy * dy);
        if (l > 1 && (cosf(a) * dx + sinf(a) * dy) / l < 0.3f) return false;   // VIEW_FIELD_WIDE-ish
    }
    return game_visible(eye, tp);
}

// look for the nearest hated, visible target
static int look_for_enemy(npc_t *n)
{
    int best = -1;
    float bd = 1e30f;
    bool fov = n->ai == AI_IDLE && !(n->flags & NF_STATIC);
    if (hates(n, 0) && player_in_pvs(n)) {
        float d = dist3(n->origin, player_pos);
        if (d < 2048 && can_see(n, 0, fov)) { best = 0; bd = d; }
    }
    // other monsters: only for allies, soldiers and machines (aliens go for humans too)
    for (int i = 0; i < num_npcs; i++) {
        npc_t *t = &npcs[i];
        if (t == n || !npc_alive(i)) continue;
        float d = dist3(n->origin, t->origin);
        if (d >= bd || d > 1500) continue;
        if (!hates(n, i + 1)) continue;
        if (can_see(n, i + 1, fov)) { best = i + 1; bd = d; }
    }
    return best;
}

void npc_hear(const float *origin, float radius, int source)
{
    for (int i = 0; i < num_npcs; i++) {
        npc_t *n = &npcs[i];
        if (npc_alive(i) && dist3(n->origin, origin) <= radius) ai_trigger(n, 9);   // AITRIGGER_HEARCOMBAT
        if (!npc_alive(i) || (n->flags & NF_NOAI) || n->script >= 0) continue;
        if (dist3(n->origin, origin) > radius) continue;
        if (n->ai == AI_IDLE && klass(n) != C_ALLY && klass(n) != C_NONE) {
            // turn towards the noise; the next look may find the enemy
            n->yaw = yaw_to(n->origin, origin);
            n->next_look = 0;
            if (source == 0 && hates(n, 0)) { n->enemy = 0; memcpy(n->last_seen, origin, 12); n->ai = AI_COMBAT; }
        }
        if (klass(n) == C_ALLY && (n->flags & NF_SCI) && n->ai == AI_IDLE) n->fear_time = game_time();
    }
}

// ---------------------------------------------------------------- damage
static void npc_die(npc_t *n, int dmgtype, const float *dir);

#ifndef DMG_ALWAYSGIB_N64
#define DMG_ALWAYSGIB_N64 (1 << 13)
#endif
void game_effect_blood(const float *o);
void game_spawn_gibs(const float *o, int count, bool alien, float speed);
void game_effect_blood_color(const float *o, bool yellow);
// BloodColor(): aliens bleed yellow and leave alien gibs; machines don't bleed
static bool npc_alien(npc_t *n)
{
    const char *m = n->model ? n->model->name : "";
    static const char *al[] = {"zombie", "headcrab", "houndeye", "bullsquid", "agrunt", "islave", "garg", "controller",
                               "bigmom", "babycrab", "icky", "leech", "tentacle", "nihilanth", "gonome", "bullchicken", "baby_headcrab"};
    for (unsigned i = 0; i < sizeof(al) / sizeof(al[0]); i++) if (strstr(m, al[i])) return true;
    return false;
}
static bool npc_bleeds(npc_t *n)
{
    const char *m = n->model ? n->model->name : "";
    return !strstr(m, "turret") && !strstr(m, "sentry") && !strstr(m, "apache") && !strstr(m, "osprey") && !strstr(m, "holo");
}
bool npc_blood(int i, bool *yellow)
{
    if (i < 0 || i >= num_npcs || !npc_bleeds(&npcs[i])) return false;
    *yellow = npc_alien(&npcs[i]);
    return true;
}
void game_effect_particles(const float *o, int count, float speed, int r, int g, int b, float life, float grav);
// CBaseMonster::GibMonster: the body bursts -- blood, gib spray, gone
static void npc_gib(npc_t *n)
{
    float o[3] = {n->origin[0], n->origin[1], n->origin[2] + 24};
    if (!npc_bleeds(n)) game_effect_particles(o, 24, 220, 200, 180, 120, 1.0f, 600);   // machines: sparks
    else game_spawn_gibs(o, 8, npc_alien(n), 200);
    snd_emit(n->ent, CHAN_BODY, "common/bodysplat.wav", 1.0f, ATTN_NORM, PITCH_NORM, NULL);
    entities[n->ent].removed = true;
    n->solid = false;
    n->flags |= NF_DEAD | NF_NOTSOLID;
    n->health = 0;
}
// CBaseMonster::FCheckAITrigger: fire the monster's TriggerTarget once when its condition happens
static void ai_trigger(npc_t *n, int cond)
{
    if (n->trig_cond != cond) return;
    n->trig_cond = -1;
    const char *tt = ent_value(&entities[n->ent], "TriggerTarget");
    if (tt) ents_fire_targets(tt);
}
#ifndef DMG_DROWN_N64
#define DMG_DROWN_N64 (1 << 14)
#endif
static float hit_point[3];
static bool hit_valid;
void npc_set_hitpoint(const float *p) { memcpy(hit_point, p, 12); hit_valid = true; }
void npc_take_damage(int i, float dmg, int dmgtype, const float *dir, int attacker)
{
    if (i < 0 || i >= num_npcs) return;
    npc_t *n = &npcs[i];
    entity_t *ne = &entities[n->ent];
    // blood (TraceAttack: SpawnBlood + TraceBleed) at the hit point, or the body's middle
    if (npc_bleeds(n) && !ne->removed && !(n->flags & NF_ITEM) && dmg > 0 && !(dmgtype & (DMG_DROWN_N64))) {
        float bp[3];
        if (hit_valid) memcpy(bp, hit_point, 12);
        else for (int k = 0; k < 3; k++) bp[k] = n->origin[k] + (n->mins[k] + n->maxs[k]) * 0.5f;
        game_effect_blood_color(bp, npc_alien(n));
    }
    hit_valid = false;
    // corpses (monster_*_dead, or already dead): never get up again; enough damage, a laser or an
    // explosion turns them into gibs (CBaseMonster::TakeDamage on a DEAD_DEAD monster -> GibMonster)
    bool corpse = strstr(ne->classname, "_dead") != NULL || is_dead(n);
    if (corpse) {
        if (ne->removed || (n->flags & NF_ITEM)) return;
        n->health -= dmg;
        if (n->health <= -10 || (dmgtype & (DMG_ENERGYBEAM | DMG_BLAST | DMG_ALWAYSGIB_N64)) || dmg >= 30) npc_gib(n);   // about three crowbar hits
        return;
    }
    if (!npc_alive(i) || (n->flags & NF_INVULN)) return;
    const mdef_t *d = D(n);
    // the gargantua shrugs off everything but explosives and energy
    if (d && !strcmp(d->model, "garg") && !(dmgtype & (DMG_BLAST | DMG_ENERGYBEAM | DMG_SHOCK))) dmg *= 0.05f;
    n->health -= dmg;
    ai_trigger(n, 2);                                             // AITRIGGER_TAKEDAMAGE
    if (n->health <= n->maxhealth * 0.5f) ai_trigger(n, 3);       // AITRIGGER_HALFHEALTH
    if (attacker == 0 && klass(n) == C_ALLY) {
        // shooting a friend: they get angry after a second hit (CTalkMonster::TakeDamage)
        if (++n->hits_by_player >= 2 && !(n->flags & NF_SCI)) {
            n->provoked = true;
            if (n->following) stop_following(n, false);
            npc_say(n, "BA_MAD", 3, 1, ATTN_NORM);
        } else if (n->health > 0) {
            npc_say(n, grp(n, "SC_PLFEAR", "BA_SHOT"), 3, 1, ATTN_NORM);
        }
    }
    if (n->health <= -30 && npc_bleeds(n) && !(n->flags & NF_STATIC)) { npc_die(n, dmgtype, dir); npc_gib(n); return; }
    if (n->health <= 0) { npc_die(n, dmgtype, dir); return; }
    // pain: sound, flinch now and then
    if (game_time() > n->pain_time) {
        n->pain_time = game_time() + 0.5f + (rand() % 100) / 100.0f;
        if (d && d->snd) npc_sound(n, CHAN_VOICE, d->snd->pain, d->snd->np, 1.0f, ATTN_NORM);
        if (n->script < 0 && n->ai != AI_ATTACK && !(n->flags & NF_STATIC) && dmg > 5 && rand() % 2 == 0) {
            if (set_activity(n, ACT_SMALL_FLINCH)) { restart_seq(n, n->seq); n->ai = AI_PAIN; }
        }
    }
    // being hurt makes enemies of whoever did it
    if (n->script < 0 && attacker >= 0 && attacker != i + 1 && hates(n, attacker)) {
        n->enemy = attacker;
        target_pos(attacker, n->last_seen);
        if (n->ai == AI_IDLE) n->ai = AI_COMBAT;
    } else if (n->script < 0 && attacker == 0 && (n->flags & NF_SCI)) {
        n->fear_time = game_time();
    }
}

static void npc_die(npc_t *n, int dmgtype, const float *dir)
{
    const mdef_t *d = D(n);
    if (d && d->snd && d->snd->die) npc_sound(n, CHAN_VOICE, d->snd->die, d->snd->nd, 1.0f, ATTN_NORM);
    n->ai = AI_DYING;
    n->enemy = -1;
    n->following = false;
    if (n->script >= 0) { n->script = -1; n->sstate = NS_IDLE; }
    snd_stop(n->ent, CHAN_VOICE, NULL);
    int act = ACT_DIESIMPLE;
    if (dir) {
        float a = n->yaw * (float)M_PI / 180;
        float f = cosf(a) * dir[0] + sinf(a) * dir[1];
        act = f > 0.3f ? ACT_DIEFORWARD : (f < -0.3f ? ACT_DIEBACKWARD : ACT_DIESIMPLE);
    }
    if (!set_activity(n, act) && !set_activity(n, ACT_DIESIMPLE) && !set_activity(n, ACT_DIEBACKWARD)) {
        n->ai = AI_DEAD;
    } else {
        restart_seq(n, n->seq);
    }
    n->solid = false;
    n->dead_time = game_time();
    // soldiers drop their gun (CHGrunt::... DropItem)
    if (d && !strcmp(d->model, "hgrunt")) {
        float o[3] = {n->origin[0], n->origin[1], n->origin[2]};
        npc_spawn_item((n->body / 4) % 4 == 3 || (n->body & 8) ? "weapon_shotgun" : "weapon_9mmAR", o, n->yaw);
    }
    entity_t *e = &entities[n->ent];
    if (e->target[0]) ents_fire_entity_target(e);   // a monster's "TriggerTarget" on death
    const char *tt = ent_value(e, "TriggerTarget");
    int cond = ent_value(e, "TriggerCondition") ? atoi(ent_value(e, "TriggerCondition")) : 0;
    if (tt && cond == 4 && n->trig_cond == 4) { n->trig_cond = -1; ents_fire_targets(tt); }   // AITRIGGER_DEATH
}

// ---------------------------------------------------------------- attacks
static void fire_bullet_at(npc_t *n, int target, float dmg, float spread, int shots)
{
    float eye[3], tp[3];
    npc_eye(n, eye);
    eye[2] -= 6;
    target_pos(target, tp);
    {   // the muzzle flash (MuzzleFlash / EF_MUZZLEFLASH) a little in front of the gun
        extern void game_effect_muzzleflash(const float *o, float rad);
        float d[3] = {tp[0] - eye[0], tp[1] - eye[1], tp[2] - eye[2]};
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (n->att_time > 0 && game_time() - n->att_time < 0.25f) game_effect_muzzleflash(n->att[0], 9);   // the gun's muzzle attachment
        else if (l > 1) { float m[3] = {eye[0] + d[0] / l * 28, eye[1] + d[1] / l * 28, eye[2] + d[2] / l * 28 - 4}; game_effect_muzzleflash(m, 9); }
    }
    for (int s = 0; s < shots; s++) {
        float dir[3] = {tp[0] - eye[0], tp[1] - eye[1], tp[2] - eye[2]};
        float l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (int k = 0; k < 3; k++) dir[k] = dir[k] / l + ((rand() % 200) - 100) / 100.0f * spread;
        float end[3] = {eye[0] + dir[0] * 4096, eye[1] + dir[1] * 4096, eye[2] + dir[2] * 4096};
        gtrace_t tr;
        game_trace(eye, end, NULL, NULL, 0, n - npcs, &tr);
        game_effect_tracer(eye, tr.endpos);
        if (tr.player) player_take_damage(dmg, DMG_BULLET, eye, n->ent);
        else if (tr.npc >= 0) npc_take_damage(tr.npc, dmg, DMG_BULLET, dir, (n - npcs) + 1);
        else if (tr.ent > 0) ents_damage_brush(tr.ent, dmg, DMG_BULLET, dir);
    }
}

static void damage_target(npc_t *n, int target, float dmg, int type)
{
    float o[3];
    npc_center(n, o);
    if (target == 0) player_take_damage(dmg, type, o, n->ent);
    else npc_take_damage(target - 1, dmg, type, NULL, (n - npcs) + 1);
}

static float target_dist(npc_t *n, int target)
{
    float tp[3];
    target_pos(target, tp);
    return dist3(n->origin, tp) - (target == 0 ? 16 : 0);
}

// the moment an attack connects (the animation's own event in the SDK)
static void attack_hit(npc_t *n)
{
    const mdef_t *d = D(n);
    if (!d || n->enemy < 0) return;
    int t = n->enemy;
    float dist = target_dist(n, t);
    if (n->attack_kind == 1) {
        // melee: claws, kicks, bites
        if (dist <= d->melee_range + 24) {
            damage_target(n, t, skill_get(d->melee_dmg), DMG_SLASH);
            if (!strcmp(d->model, "zombie")) snd_emit(n->ent, CHAN_WEAPON, rand() & 1 ? "zombie/claw_strike1.wav" : "zombie/claw_strike2.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
            if (t == 0) { float p[3] = {5, (rand() % 20) - 10, 0}; pm_set_punch(p); }
        } else if (!strcmp(d->model, "zombie")) {
            snd_emit(n->ent, CHAN_WEAPON, rand() & 1 ? "zombie/claw_miss1.wav" : "zombie/claw_miss2.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        }
        return;
    }
    float dmg = d->ranged_dmg ? skill_get(d->ranged_dmg) : 0;
    float eye[3], tp[3];
    npc_eye(n, eye);
    target_pos(t, tp);
    switch (d->ranged) {
    case R_BULLET: {
        bool grunt = !strcmp(d->model, "hgrunt");
        bool turret = (n->flags & NF_STATIC) != 0;
        if (!strcmp(d->model, "barney")) snd_emit(n->ent, CHAN_WEAPON, "barney/ba_attack2.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        else if (grunt) { char s[32]; snprintf(s, sizeof(s), "hgrunt/gr_mgun%d.wav", 1 + rand() % 3); snd_emit(n->ent, CHAN_WEAPON, s, 1, ATTN_NORM, PITCH_NORM, NULL); }
        else if (turret) snd_emit(n->ent, CHAN_WEAPON, "turret/tu_fire1.wav", 0.6f, 0.6f, PITCH_NORM, NULL);
        else snd_emit(n->ent, CHAN_WEAPON, "weapons/pl_gun1.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        fire_bullet_at(n, t, dmg, grunt ? 0.06f : 0.03f, grunt ? 3 : 1);
        npc_hear(n->origin, 600, (n - npcs) + 1);
        break;
    }
    case R_ZAP:
        snd_emit(n->ent, CHAN_WEAPON, "debris/zap4.wav", 1, ATTN_NORM, 130, NULL);
        game_effect_beam(eye, tp, 96, 128, 16);
        if (can_see(n, t, false)) damage_target(n, t, dmg, DMG_SHOCK);
        break;
    case R_SONIC: {
        // CHoundeye::SonicAttack: everything around takes damage, falling off with distance
        char s[32];
        snprintf(s, sizeof(s), "houndeye/he_blast%d.wav", 1 + rand() % 3);
        snd_emit(n->ent, CHAN_WEAPON, s, 1, ATTN_NORM, PITCH_NORM, NULL);
        float c[3];
        npc_center(n, c);
        float pd = dist3(c, player_pos);
        if (pd < 384 && hates(n, 0)) player_take_damage(dmg * (1 - pd / 384), DMG_SONIC, c, n->ent);
        for (int i = 0; i < num_npcs; i++) {
            if (&npcs[i] == n || !npc_alive(i) || !strcmp(D(&npcs[i]) ? D(&npcs[i])->model : "", "houndeye")) continue;
            float dd = dist3(c, npcs[i].origin);
            if (dd < 384) npc_take_damage(i, dmg * (1 - dd / 384), DMG_SONIC, NULL, (n - npcs) + 1);
        }
        break;
    }
    case R_LEAP: {
        // CHeadCrab::LeapTouch: jump at the target; the bite lands if we reach it
        float dir[3] = {tp[0] - n->origin[0], tp[1] - n->origin[1], 0};
        float l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
        if (l > 1) {
            float g = 800, t2 = l / 350;
            n->vz = -(tp[2] - 16 - n->origin[2]) / (t2 > 0.1f ? t2 : 0.1f) - 0.5f * g * t2;
            n->onground = false;
            n->path[0][0] = dir[0] / l * 350; n->path[0][1] = dir[1] / l * 350;   // leap velocity
            n->stuck_time = -1;
        }
        npc_sound(n, CHAN_VOICE, d->snd->atk, d->snd->nat, 1, ATTN_IDLE);
        break;
    }
    case R_SPIT:
    case R_HORNET:
    case R_BALL: {
        float dir[3] = {tp[0] - eye[0], tp[1] - eye[1], tp[2] - eye[2]};
        float l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (int k = 0; k < 3; k++) dir[k] /= l > 0 ? l : 1;
        int kind = d->ranged == R_SPIT ? 10 : (d->ranged == R_HORNET ? 11 : 12);
        proj_spawn(kind, eye, dir, (n - npcs) + 1, t);
        if (d->ranged == R_HORNET) { char s[32]; snprintf(s, sizeof(s), "agrunt/ag_fire%d.wav", 1 + rand() % 3); snd_emit(n->ent, CHAN_WEAPON, s, 1, ATTN_NORM, PITCH_NORM, NULL); }
        else if (d->ranged == R_SPIT) snd_emit(n->ent, CHAN_WEAPON, "bullchicken/bc_attack2.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        else npc_sound(n, CHAN_WEAPON, d->snd->atk, d->snd->nat, 1, ATTN_NORM);
        break;
    }
    case R_FLAME:
        if (dist < d->rmax) damage_target(n, t, dmg * 3, DMG_BURN);
        break;
    }
}

// pick and start an attack if one is possible
static bool try_attack(npc_t *n, float dist, bool visible)
{
    const mdef_t *d = D(n);
    float now = game_time();
    if (!d || now < n->next_attack || !visible) return false;
    if (d->melee_dmg && dist <= d->melee_range) {
        if (!set_activity(n, ACT_MELEE_ATTACK1)) return false;
        restart_seq(n, n->seq);
        n->attack_kind = 1;
        n->ai = AI_ATTACK;
        n->attack_hit_done = false;
        n->attack_hit_time = now + studio_seq_duration(n->model, n->seq) * 0.4f;
        n->next_attack = now + 0.8f;
        if (d->snd && d->snd->atk && d->ranged != R_LEAP) npc_sound(n, CHAN_VOICE, d->snd->atk, d->snd->nat, 1, ATTN_IDLE);
        return true;
    }
    if (d->ranged != R_NONE && dist >= d->rmin && dist <= d->rmax) {
        int act = ACT_RANGE_ATTACK1;
        if (d->ranged == R_LEAP && !n->onground) return false;
        if (!set_activity(n, act) && !set_activity(n, ACT_MELEE_ATTACK1)) {
            if (!(n->flags & NF_STATIC)) return false;
        }
        restart_seq(n, n->seq);
        n->attack_kind = 2;
        n->ai = AI_ATTACK;
        n->attack_hit_done = false;
        float dur = studio_seq_duration(n->model, n->seq);
        n->attack_hit_time = now + (d->ranged == R_LEAP ? dur * 0.3f : dur * 0.35f);
        n->next_attack = now + d->rcool + (rand() % 100) / 200.0f;
        if (n->flags & NF_STATIC) { n->attack_hit_time = now; n->ai = AI_COMBAT; attack_hit(n); }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- talking monsters
static void start_following(npc_t *n)
{
    n->following = true;
    entity_t *e = &entities[n->ent];
    const char *us = ent_value(e, "UseSentence");
    char s[48];
    if (us) { strncpy(s, us, 47); s[47] = 0; npc_say(n, s, 3, 1, ATTN_IDLE); }
    else npc_say(n, grp(n, "SC_OK", "BA_OK"), 3, 1, ATTN_IDLE);
    n->talk_target = 0;
    n->said |= 1;
}

static void stop_following(npc_t *n, bool speak)
{
    if (!n->following) return;
    n->following = false;
    n->npath = 0;
    if (speak && !n->provoked) {
        entity_t *e = &entities[n->ent];
        const char *us = ent_value(e, "UnUseSentence");
        char s[48];
        if (us) { strncpy(s, us, 47); s[47] = 0; npc_say(n, s, 3, 1, ATTN_IDLE); }
        else npc_say(n, grp(n, "SC_WAIT", "BA_WAIT"), 3, 1, ATTN_IDLE);
        n->talk_target = 0;
    }
    if (n->ai == AI_IDLE) set_seq(n, n->idle_seq);
}

int npc_find_usable(const float *eye, const float *forward, float *bestdot)
{
    int best = -1;
    for (int i = 0; i < num_npcs; i++) {
        npc_t *n = &npcs[i];
        if (!(n->flags & NF_TALKER) || !npc_alive(i)) continue;
        float c[3];
        npc_center(n, c);
        float d[3] = {c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]};
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l > 64 + 32 || l < 1) continue;   // PLAYER_SEARCH_RADIUS from the monster's edge
        float dot = (d[0] * forward[0] + d[1] * forward[1] + d[2] * forward[2]) / l;
        if (dot > *bestdot) { *bestdot = dot; best = i; }
    }
    return best;
}

// CTalkMonster::FollowerUse
void npc_player_use(int i)
{
    npc_t *n = &npcs[i];
    float now = game_time();
    if (n->use_time > now) return;               // not during a scripted sentence
    if (n->flags & NF_PREDIS) {
        // DeclineFollowing
        npc_say(n, grp(n, "SC_POK", "BA_POK"), 2, 1, ATTN_NORM);
        n->talk_target = 0;
        n->use_time = now + 1.0f;
        return;
    }
    if (n->provoked) return;
    bool can_follow = !n->following && (n->script < 0 || !(entities[n->script].spawnflags & 32));
    if (can_follow) {
        // LimitFollowers(player, 1): only one follower of each kind
        for (int k = 0; k < num_npcs; k++)
            if (k != i && npcs[k].following && D(&npcs[k]) && D(n) && !strcmp(D(&npcs[k])->model, D(n)->model))
                stop_following(&npcs[k], true);
        if (n->script >= 0) { n->script = -1; n->sstate = NS_IDLE; }
        start_following(n);
    } else {
        stop_following(n, true);
    }
}

// idle chatter (CTalkMonster::GetScheduleOfType SCHED_IDLE_STAND / FIdleHello / FIdleSpeak)
static void talker_idle(npc_t *n, float dt)
{
    float now = game_time();
    if (n->flags & NF_GAG) return;
    float pd = dist3(n->origin, player_pos);
    bool see_player = pd < 512 && player_alive() && can_see(n, 0, false);
    if (!(n->said & 1)) {
        if (pd < 256 && see_player && ok_to_speak(n)) {
            const char *g = (n->flags & NF_PREDIS) ? grp(n, "SC_PHELLO", NULL) : grp(n, "SC_HELLO", "BA_HELLO");
            if (g) npc_say(n, g, 3.2f, 1, ATTN_IDLE);
            n->said |= 1;
            n->talk_target = 0;
        }
        return;
    }
    if (now < n->idle_speak_time) return;
    n->idle_speak_time = now + 3 + (rand() % 100) / 25.0f;
    if (!ok_to_speak(n) || rand() % (n->nspeak * 2 + 1) != 0) return;
    // hurt player being followed
    if (n->following && player.health > 0) {
        const char *s = NULL;
        if (!(n->said & 8) && player.health <= 100 / 8) { s = grp(n, "!SC_CUREC", "!BA_CUREC"); n->said |= 8; }
        else if (!(n->said & 4) && player.health <= 100 / 4) { s = grp(n, "!SC_CUREB", "!BA_CUREB"); n->said |= 4; }
        else if (!(n->said & 2) && player.health <= 100 / 2) { s = grp(n, "!SC_CUREA", "!BA_CUREA"); n->said |= 2; }
        if (s) { npc_say(n, s, 3, 1, ATTN_IDLE); n->talk_target = 0; return; }
    }
    bool pre = (n->flags & NF_PREDIS) != 0;
    const char *idle = pre ? grp(n, "SC_PIDLE", NULL) : grp(n, "SC_IDLE", "BA_IDLE");
    const char *quest = pre ? grp(n, "SC_PQUEST", "BA_PQUEST") : grp(n, "SC_QUESTION", "BA_QUESTION");
    float dur = pre ? 5 : 3;
    // a friend nearby to talk to?
    for (int k = 0; k < num_npcs; k++) {
        npc_t *f = &npcs[k];
        if (f == n || !(f->flags & NF_TALKER) || !npc_alive(k) || f->script >= 0 || dist3(f->origin, n->origin) > 256) continue;
        if (rand() % 100 >= 75) break;
        npc_say(n, quest, dur, 1, ATTN_IDLE);
        n->talk_target = f->ent;
        n->nspeak++;
        // the friend answers after we finish (SetAnswerQuestion)
        f->idle_speak_time = 0;
        f->talk_target = n->ent;
        f->said |= 16;          // pending answer
        f->heal_time = now + dur;
        return;
    }
    if (rand() & 1 && see_player && idle) {
        npc_say(n, idle, dur, 1, ATTN_IDLE);
        n->talk_target = 0;
        n->nspeak++;
        return;
    }
    talk_wait_time = 0;
}

// ---------------------------------------------------------------- per-frame AI
static void face_yaw(npc_t *n, float want, float rate, float dt)
{
    float dy = angle_diff(want, n->yaw);
    float mt = rate * dt;
    n->yaw += dy > mt ? mt : (dy < -mt ? -mt : dy);
}

static void ai_idle(npc_t *n, float dt)
{
    float now = game_time();
    int i = n - npcs;
    const mdef_t *d = D(n);
    // following the player
    if (n->following) {
        float pd = dist2d(n->origin, player_pos);
        if (pd > 128 || fabsf(n->origin[2] - (player_pos[2] - 36)) > 64) {
            bool run = pd > 300;
            if (n->npath == 0 || dist2d(n->goal, player_pos) > 64 || n->stuck_time > 1.0f) {
                float g[3] = {player_pos[0], player_pos[1], player_pos[2] - 36};
                start_moving(n, g, run);
            }
            if (n->running != run) { n->running = run; n->speed = move_speed(n, run); set_activity(n, run ? ACT_RUN : ACT_WALK); }
            if (follow_route(n, n->speed, dt, 100)) { n->npath = 0; set_seq(n, n->idle_seq); }
            return;
        }
        if (n->npath) { n->npath = 0; set_seq(n, n->idle_seq); }
        face_yaw(n, yaw_to(n->origin, player_pos), 180, dt);
    }
    // scientists: heal a hurt leader (CScientist::CanHeal / Heal)
    if ((n->flags & NF_SCI) && n->following && player.health > 0 && player.health <= player_max_health() * 0.5f &&
        now > n->heal_time && dist2d(n->origin, player_pos) < 96) {
        n->heal_time = now + 60;
        npc_say(n, "SC_HEAL", 2, 1, ATTN_IDLE);
        player_take_health(skill_get("sk_scientist_heal"), DMG_GENERIC);
    }
    // answering a question
    if ((n->said & 16) && now > n->heal_time && n->heal_time < now + 1e5f) {
        n->said &= ~16;
        if (!(n->flags & NF_GAG) && !is_dead(n)) npc_say(n, grp(n, "SC_ANSWER", "BA_ANSWER"), 3, 1, ATTN_IDLE);
    }
    // face whoever we're talking to
    if (n->talk_target >= 0 && now < n->talk_time && !n->following && !(n->flags & NF_STATIC)) {
        float tp[3];
        if (n->talk_target == 0) memcpy(tp, player_pos, 12);
        else ents_sound_origin(n->talk_target, tp);
        face_yaw(n, yaw_to(n->origin, tp), 120, dt);
    }
    if (n->flags & NF_TALKER) talker_idle(n, dt);
    else if (d && d->snd && d->snd->idle && now > n->idle_speak_time) {
        n->idle_speak_time = now + 5 + rand() % 15;
        if (player_in_pvs(n) && dist3(n->origin, player_pos) < 800) npc_sound(n, CHAN_VOICE, d->snd->idle, d->snd->ni, 1, ATTN_IDLE);
    }
    (void)i;
}

static void ai_combat(npc_t *n, float dt)
{
    const mdef_t *d = D(n);
    float now = game_time();
    int t = n->enemy;
    if (t < 0 || (t == 0 && !hates(n, 0)) || (t > 0 && !npc_alive(t - 1))) {
        n->enemy = -1;
        n->ai = n->provoked && t == 0 ? AI_COMBAT : AI_IDLE;
        if (n->ai == AI_IDLE) { set_seq(n, n->idle_seq); n->npath = 0; }
        if (t < 0) n->ai = AI_IDLE;
        return;
    }
    float tp[3];
    target_pos(t, tp);
    bool vis = can_see(n, t, false);
    if (vis) memcpy(n->last_seen, tp, 12);
    float dist = target_dist(n, t);
    // scientists run from danger
    if (klass(n) == C_ALLY && (n->flags & NF_SCI)) { n->ai = AI_FLEE; n->fear_time = now; return; }
    // face the enemy while attacking / standing
    if (try_attack(n, dist, vis)) {
        n->yaw = yaw_to(n->origin, tp);
        return;
    }
    if (n->flags & NF_STATIC) { face_yaw(n, yaw_to(n->origin, tp), 180, dt); return; }
    // chase (or keep distance for ranged attackers that can see us)
    bool ranged_ok = d && d->ranged != R_NONE && d->ranged != R_LEAP && vis && dist <= d->rmax && dist >= d->rmin;
    if (ranged_ok) {
        face_yaw(n, yaw_to(n->origin, tp), 240, dt);
        if (!set_activity(n, ACT_COMBAT_IDLE) && !set_activity(n, ACT_IDLE_ANGRY)) set_activity(n, ACT_IDLE);
        n->npath = 0;
        return;
    }
    float stop = d && d->melee_dmg ? d->melee_range * 0.8f : 48;
    if (dist <= stop && vis) { face_yaw(n, yaw_to(n->origin, tp), 300, dt); set_activity(n, ACT_IDLE); return; }
    if (n->npath == 0 || dist3(n->goal, n->last_seen) > 96 || n->stuck_time > 1.0f || n->pathi >= n->npath) {
        float g[3] = {n->last_seen[0], n->last_seen[1], n->last_seen[2] - (t == 0 ? 36 : 0)};
        start_moving(n, g, true);
    }
    if (follow_route(n, n->speed, dt, stop)) {
        n->npath = 0;
        if (!vis && dist3(n->origin, n->last_seen) < 96) { n->enemy = -1; n->ai = AI_IDLE; set_seq(n, n->idle_seq); }
    }
}

static void ai_flee(npc_t *n, float dt)
{
    float now = game_time();
    if (now - n->fear_time > 8) { n->ai = AI_IDLE; n->enemy = -1; n->npath = 0; set_seq(n, n->idle_seq); return; }
    if (n->enemy >= 0 && (n->enemy == 0 || npc_alive(n->enemy - 1))) {
        float tp[3];
        target_pos(n->enemy, tp);
        if (dist3(n->origin, tp) > 600 && !can_see(n, n->enemy, false)) { if (!set_activity(n, ACT_COWER)) set_activity(n, ACT_IDLE); return; }
        if (n->npath == 0 || n->pathi >= n->npath || n->stuck_time > 1) {
            // run to the node furthest from the danger among the nearby ones (CoverRadius)
            int best = -1;
            float bd = 0;
            for (int k = 0; k < nav_num_nodes(); k++) {
                const float *o = nav_node_origin(k);
                float dn = dist3(o, n->origin);
                if (dn > 800 || dn < 64) continue;
                float de = dist3(o, tp);
                if (de > bd) { bd = de; best = k; }
            }
            if (best < 0) { if (!set_activity(n, ACT_COWER)) set_activity(n, ACT_IDLE); return; }
            if (n->said & 32) {} else { n->said |= 32; npc_say(n, "SC_SCREAM", 2, 1, ATTN_NORM); }
            start_moving(n, nav_node_origin(best), true);
            if (!set_activity(n, ACT_RUN_SCARED)) set_activity(n, ACT_RUN);
        }
        if (follow_route(n, n->speed * 1.2f, dt, 16)) { n->npath = 0; if (!set_activity(n, ACT_COWER)) set_activity(n, ACT_IDLE); }
    } else {
        n->ai = AI_IDLE;
        set_seq(n, n->idle_seq);
    }
}

// barnacle (CBarnacle::BarnacleThink): the tongue hangs down to the floor (TongueTouchEnt traces
// 2048 units down), grows back at BARNACLE_PULL_SPEED after letting go, grabs the player who
// touches it and reels them up into the mouth
static void barnacle_think(npc_t *n, float dt)
{
    gtrace_t tr;
    float st[3] = {n->origin[0], n->origin[1], n->origin[2] - 2};
    float end[3] = {n->origin[0], n->origin[1], n->origin[2] - 2048};
    game_trace(st, end, NULL, NULL, TR_IGNORE_MONSTERS | TR_IGNORE_PLAYER, n - npcs, &tr);
    if (n->ctl0 == 0) n->alt = fabsf(n->origin[2] - tr.endpos[2]);   // first think: already hanging down
    float len = fabsf(n->origin[2] - tr.endpos[2]);
    float dx = player_pos[0] - n->origin[0], dy = player_pos[1] - n->origin[1];
    float below = n->origin[2] - player_pos[2];
    // player hull top is 36 above the origin (standing): the tongue touches them when it reaches that
    bool touch = player_alive() && !pm_noclip_active() && dx * dx + dy * dy < 20 * 20 && below > 0 && below - 36 < n->alt;
    if (touch) {
        if (n->ai != AI_GRABBED) {
            n->ai = AI_GRABBED;
            snd_emit(n->ent, CHAN_WEAPON, "barnacle/bcl_alert2.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        }
        // pull the player up into the mouth, then chew
        float v[3] = {-dx * 4, -dy * 4, below > 56 ? 40.0f : 0.0f};
        pm_set_velocity(v);
        n->alt = fmaxf(below - 36, 0);          // the tongue is as long as the prey is low
        if (below <= 60 && game_time() > n->next_attack) {
            n->next_attack = game_time() + 1.2f;
            npc_sound(n, CHAN_WEAPON, "barnacle/bcl_chew%d.wav", 3, 1, ATTN_NORM);
            player_take_damage(10, DMG_SLASH | DMG_ALWAYSGIB, n->origin, n->ent);
        }
    } else {
        if (n->ai == AI_GRABBED) n->ai = AI_IDLE;
        if (n->alt < len) n->alt = fminf(n->alt + 80 * dt, len);   // BARNACLE_PULL_SPEED 8 per 0.1 s
        else n->alt = len;
    }
    n->ctl0 = -(n->alt - 100);                 // SetBoneController(0, -(m_flAltitude + m_flTongueAdj)), adj = -100
}

// tentacles strike at things that make noise nearby (CTentacle, simplified)
static void tentacle_think(npc_t *n, float dt)
{
    float pd = dist2d(n->origin, player_pos);
    float now = game_time();
    if (pd < 380 && player_alive() && now > n->next_attack && !pm_noclip_active()) {
        float v[3];
        pm_get_velocity(v);
        bool noisy = sqrtf(v[0] * v[0] + v[1] * v[1]) > 160 || !pm_ducking();
        if (noisy && player_pos[2] > n->origin[2] - 64) {
            n->next_attack = now + 2.5f;
            if (!set_activity(n, ACT_MELEE_ATTACK1)) set_activity(n, ACT_RANGE_ATTACK1);
            restart_seq(n, n->seq);
            n->ai = AI_ATTACK;
            n->enemy = 0;
            n->attack_kind = 1;
            n->attack_hit_done = false;
            n->attack_hit_time = now + 0.6f;
            npc_sound(n, CHAN_VOICE, "tentacle/te_strike%d.wav", 2, 1, ATTN_NORM);
        }
    }
}

static void item_think(npc_t *n, int i)
{
    if (!player_alive()) return;
    float pmn[3], pmx[3];
    pm_get_hull(pmn, pmx);
    for (int k = 0; k < 3; k++) {
        if (player_pos[k] + pmn[k] > n->origin[k] + n->maxs[k]) return;
        if (player_pos[k] + pmx[k] < n->origin[k] + n->mins[k]) return;
    }
    entity_t *e = &entities[n->ent];
    const char *cls = D(n) ? D(n)->cls : e->classname;
    if (player_touch_item(cls, i)) {
        e->removed = true;
        ents_fire_entity_target(e);   // CItem::ItemTouch / DefaultTouch: SUB_UseTargets
    }
}

void npcs_think(float dt)
{
    float now = game_time();
    pm_get_origin(player_pos);
    player_get_eye(player_eye);
    // the player's PVS, for "can the player see me" checks (FIND_CLIENT_IN_PVS)
    int pl = bsp_leaf_for_point(player_eye);
    const uint8_t *pvs = pl > 0 ? bsp_leaf_pvs(pl) : NULL;
    int bytes = (world.numleafs + 7) / 8;
    pvs_ok = pvs && bytes <= (int)sizeof(pvs_buf);
    if (pvs_ok) memcpy(pvs_buf, pvs, bytes);
    scripts_retry();
#ifdef NPCDEBUG
    { static float nd; if (now > nd) { nd = now + 1; for (int i = 0; i < num_npcs; i++) if (npcs[i].script >= 0 || npcs[i].following || npcs[i].ai)
        debugf("  npc %s at %.0f %.0f %.0f ss %d ai %d path %d/%d stuck %.1f\n", entities[npcs[i].ent].targetname, npcs[i].origin[0], npcs[i].origin[1], npcs[i].origin[2], npcs[i].sstate, npcs[i].ai, npcs[i].pathi, npcs[i].npath, npcs[i].stuck_time); } }
#endif

    extern int trace_frames;
    for (int i = 0; i < num_npcs; i++) {
        npc_t *n = &npcs[i];
        if (entities[n->ent].removed) continue;
        if (n->flags & NF_ITEM) { item_think(n, i); continue; }
        studio_t *m = n->model;
        sseq_t *s = &m->seqs[n->seq];
        float fps = s->fps > 0 ? s->fps : 10;
        int before = (int)n->frame;
        n->frame += fps * dt * n->framerate;
        if (n->frame >= s->numframes - 1) {
            n->seq_done = true;
            bool loop = (s->flags & 1) || n->sstate == NS_SCRIPT_IDLE || n->sstate == NS_MOVING || n->sstate == NS_TURNING ||
                        (n->sstate == NS_IDLE && n->ai != AI_ATTACK && n->ai != AI_PAIN && n->ai != AI_DYING && n->ai != AI_DEAD);
            if (loop && n->sstate != NS_PLAYING) n->frame = fmodf(n->frame, s->numframes > 1 ? s->numframes - 1 : 1);
            else n->frame = s->numframes - 1;
        }
        if (n->sstate == NS_PLAYING || n->ai == AI_ATTACK) fire_anim_events(n, before, (int)n->frame);

        // ---- scripted sequences take priority
        if (n->script >= 0 || n->sstate != NS_IDLE) {
            switch (n->sstate) {
            case NS_MOVING: {
                bool arrived = follow_route(n, n->speed, dt, 4);
                if (dist3(n->origin, player_pos) < 96) n->stuck_time = 0;   // waiting for the player to move aside
                if (!arrived && n->stuck_time > 1.0f) {
                    // re-plan; after a few tries put the monster on its mark so the scene goes on
                    n->stuck_time = 0;
                    if (++n->stuck_count >= 4) { debugf("npc %s stuck: teleported to its mark\n", entities[n->ent].targetname); { extern void weapons_tripmine_sweep(const float *a, const float *b, float zlo, float zhi); weapons_tripmine_sweep(n->origin, n->dest, n->mins[2], n->maxs[2]); } memcpy(n->origin, n->dest, 12); arrived = true; }
                    else route_to(n, n->dest);
                }
                if (arrived) {
                    if (dist2d(n->origin, n->dest) < 24) { n->origin[0] = n->dest[0]; n->origin[1] = n->dest[1]; }
                    n->sstate = NS_TURNING;
                    n->npath = 0;
                    set_seq(n, n->idle_seq);
                }
                bsp_light_at(n->origin, n->light);
                break;
            }
            case NS_TURNING: {
                float dy = angle_diff(n->destyaw, n->yaw);
                float mt = 270 * dt;
                if (fabsf(dy) <= mt) { n->yaw = n->destyaw; script_play(n); }
                else n->yaw += dy > 0 ? mt : -mt;
                break;
            }
            case NS_PLAYING:
                if (n->seq_done) script_done(n);
                break;
            default:
                break;
            }
            gravity(n, dt);
            continue;
        }
        if (n->flags & (NF_NOAI | NF_DEAD)) continue;

        // AI triggers that need the player in sight (SEEPLAYER_*), checked a few times a second
        if ((n->trig_cond == 1 || n->trig_cond == 10 || n->trig_cond == 11) && now >= n->trig_check) {
            n->trig_check = now + 0.3f;
            if (can_see(n, 0, true)) ai_trigger(n, n->trig_cond);
        }
        // ---- AI
        switch (n->ai) {
        case AI_DYING:
            if (n->seq_done) { n->ai = AI_DEAD; n->frame = m->seqs[n->seq].numframes - 1; }
            gravity(n, dt);
            continue;
        case AI_DEAD:
            continue;
        case AI_PAIN:
            if (n->seq_done) { n->ai = n->enemy >= 0 ? AI_COMBAT : AI_IDLE; set_seq(n, n->idle_seq); }
            continue;
        case AI_ATTACK:
            if (!n->attack_hit_done && now >= n->attack_hit_time) { n->attack_hit_done = true; attack_hit(n); }
            if (n->stuck_time < 0) {
                // headcrab in the air: fly along the leap and bite on contact
                float v[3] = {n->path[0][0] * dt, n->path[0][1] * dt, -n->vz * dt};
                float to[3] = {n->origin[0] + v[0], n->origin[1] + v[1], n->origin[2] + v[2]};
                gtrace_t tr;
                game_trace(n->origin, to, n->mins, n->maxs, 0, i, &tr);
                if (!tr.startsolid) memcpy(n->origin, tr.endpos, 12);
                n->vz += 800 * dt;
                if (tr.player && n->enemy == 0) { damage_target(n, 0, skill_get(D(n)->ranged_dmg), DMG_SLASH); snd_emit(n->ent, CHAN_WEAPON, "headcrab/hc_headbite.wav", 1, ATTN_NORM, PITCH_NORM, NULL); n->stuck_time = 0; }
                else if (tr.npc >= 0 && n->enemy == tr.npc + 1) { damage_target(n, n->enemy, skill_get(D(n)->ranged_dmg), DMG_SLASH); n->stuck_time = 0; }
                else if (tr.fraction < 1 && tr.normal[2] > 0.7f) { n->stuck_time = 0; n->onground = true; n->vz = 0; }
                else if (tr.fraction < 1) { n->path[0][0] = n->path[0][1] = 0; }
                if (n->stuck_time == 0) { n->ai = AI_COMBAT; set_seq(n, n->idle_seq); }
                bsp_light_at(n->origin, n->light);
                continue;
            }
            if (n->seq_done) { n->ai = AI_COMBAT; set_seq(n, n->idle_seq); }
            gravity(n, dt);
            continue;
        default:
            break;
        }

        const mdef_t *d = D(n);
        if (d && !strcmp(d->model, "barnacle")) { barnacle_think(n, dt); continue; }
        if (d && !strcmp(d->model, "tentacle2")) { tentacle_think(n, dt); continue; }

        // look for enemies a few times a second
        if (now >= n->next_look) {
            n->next_look = now + 0.3f + (rand() % 100) / 500.0f;
            bool near_player = dist3(n->origin, player_pos) < 2500;
            if (near_player || n->ai != AI_IDLE) {
                int e = look_for_enemy(n);
                if (e >= 0 && (n->enemy < 0 || n->ai == AI_IDLE)) {
                    n->enemy = e;
                    target_pos(e, n->last_seen);
                    if (n->ai == AI_IDLE) {
                        n->ai = AI_COMBAT;
                        if (n->following && klass(n) == C_ALLY && !(n->flags & NF_SCI)) {}
                        if (d && d->snd && d->snd->alert) npc_sound(n, CHAN_VOICE, d->snd->alert, d->snd->na, 1, ATTN_NORM);
                        else if (d && !strcmp(d->model, "barney") && ok_to_speak(n)) npc_say(n, "BA_ATTACK", 2, 1, ATTN_NORM);
                        if (n->flags & NF_SCI) { n->ai = AI_FLEE; n->fear_time = now; n->said &= ~32; }
                    }
                } else if (e >= 0 && e != n->enemy) {
                    // switch to a closer target
                    float tp[3], tp2[3];
                    target_pos(e, tp);
                    target_pos(n->enemy, tp2);
                    if (dist3(n->origin, tp) + 128 < dist3(n->origin, tp2)) n->enemy = e;
                }
            }
        }
        switch (n->ai) {
        case AI_IDLE: ai_idle(n, dt); break;
        case AI_COMBAT: ai_combat(n, dt); break;
        case AI_FLEE: ai_flee(n, dt); break;
        default: break;
        }
        gravity(n, dt);
        if (!(n->flags & NF_STATIC)) bsp_light_at(n->origin, n->light);
    }
}

// solid NPC boxes for the player's movement code
int npcs_get_boxes(npc_box_t *out, int max, const float *near, float range)
{
    int c = 0;
    for (int i = 0; i < num_npcs && c < max; i++) {
        npc_t *n = &npcs[i];
        if (!n->solid || entities[n->ent].removed) continue;
        if (fabsf(n->origin[0] - near[0]) > range || fabsf(n->origin[1] - near[1]) > range ||
            fabsf(n->origin[2] - near[2]) > range) continue;
        memcpy(out[c].origin, n->origin, 12);
        memcpy(out[c].mins, n->mins, 12);
        memcpy(out[c].maxs, n->maxs, 12);
        c++;
    }
    return c;
}

bool npc_origin_for_entity(int ent, float *origin)
{
    int i = find_npc_for_entity(ent);
    if (i < 0) return false;
    memcpy(origin, npcs[i].origin, 12);
    return true;
}

// ---------------------------------------------------------------- level transitions
// Monsters near the landmark go with the player into the next map (CChangeLevel::ChangeList:
// entities inside the trigger_transition volume, or in the landmark's PVS when there is none).
typedef struct {
    char model[32], kv[200];
    float rel[3], yaw, mins[3], maxs[3];
    int body, skin, seq, flags, def;
    float health;
    bool following;
    uint16_t said;
} carried_t;
static carried_t carried[16];
static int num_carried;

static bool in_transition(const float *o, const char *landmark, const float *lm)
{
    bool have_volume = false;
    for (int i = ents_find_class("trigger_transition", 0); i >= 0; i = ents_find_class("trigger_transition", i + 1)) {
        entity_t *t = &entities[i];
        if (strcasecmp(t->targetname, landmark) || t->model <= 0) continue;
        have_volume = true;
        bmodel_t *bm = &world.bmodels[t->model];
        bool inside = true;
        for (int k = 0; k < 3; k++)
            if (o[k] < t->origin[k] + bm->mins[k] || o[k] > t->origin[k] + bm->maxs[k]) inside = false;
        if (inside) return true;
    }
    if (have_volume) return false;
    int l0 = bsp_leaf_for_point(lm), l1 = bsp_leaf_for_point(o);
    if (l0 <= 0 || l1 <= 0) return false;
    const uint8_t *pvs = bsp_leaf_pvs(l0);
    return pvs && (pvs[(l1 - 1) >> 3] & (1 << ((l1 - 1) & 7)));
}

void npcs_save_transition(const char *landmark, const float *lm)
{
    num_carried = 0;
    for (int i = 0; i < num_npcs && num_carried < 16; i++) {
        npc_t *n = &npcs[i];
        entity_t *e = &entities[n->ent];
        if (e->removed || !n->model || is_dead(n) || (n->flags & NF_ITEM)) continue;
        float mid[3] = {n->origin[0], n->origin[1], n->origin[2] + 16};
        if (!in_transition(mid, landmark, lm)) continue;
        carried_t *c = &carried[num_carried++];
        strncpy(c->model, n->model->name, 31);
        snprintf(c->kv, sizeof(c->kv), "\"classname\" \"%s\" \"targetname\" \"%s\" }", e->classname, e->targetname);
        for (int k = 0; k < 3; k++) c->rel[k] = n->origin[k] - lm[k];
        c->yaw = n->yaw;
        memcpy(c->mins, n->mins, 12);
        memcpy(c->maxs, n->maxs, 12);
        c->body = n->body;
        c->skin = n->skin;
        c->seq = n->idle_seq;
        c->flags = n->flags;
        c->def = n->def;
        c->health = n->health;
        c->following = n->following;
        c->said = n->said;
    }
    debugf("transition: %d characters follow the player\n", num_carried);
}

void npcs_restore_transition(const float *lm)
{
    for (int c = 0; c < num_carried && num_npcs < MAX_NPCS && num_entities < MAX_ENTITIES; c++) {
        carried_t *cr = &carried[c];
        studio_t *m = studio_get(cr->model);
        if (!m) continue;
        entity_t *e = &entities[num_entities];
        memset(e, 0, sizeof(*e));
        e->kv = cr->kv;
        strncpy(e->classname, ent_value(e, "classname"), sizeof(e->classname) - 1);
        const char *tn = ent_value(e, "targetname");
        if (tn) strncpy(e->targetname, tn, sizeof(e->targetname) - 1);
        // the next map may place the same character itself
        if (e->targetname[0] && ents_find_targetname(e->targetname, 1) >= 0) continue;
        e->type = ENT_OTHER;
        e->model = -1;
        e->renderamt = 255;
        for (int k = 0; k < 3; k++) e->origin[k] = lm[k] + cr->rel[k];
        int idx = num_entities++;
        npc_t *n = new_npc(idx, cr->def, m);
        if (!n) break;
        memcpy(n->origin, e->origin, 12);
        n->yaw = cr->yaw;
        memcpy(n->mins, cr->mins, 12);
        memcpy(n->maxs, cr->maxs, 12);
        n->flags = cr->flags;
        n->solid = !(n->flags & NF_NOTSOLID);
        n->body = cr->body;
        n->skin = cr->skin;
        n->idle_seq = cr->seq < m->numseqs ? cr->seq : 0;
        n->health = cr->health;
        n->maxhealth = cr->health;
        n->following = cr->following;
        n->said = cr->said;
        n->pitch = head_pitch(n);
        set_seq(n, n->idle_seq);
        bsp_light_at(n->origin, n->light);
    }
    num_carried = 0;
}

// ------------------------------------------------------------------ save-game snapshot of the characters
static int base_npcs, base_ents;
void npcs_mark_base(void) { base_npcs = num_npcs; base_ents = num_entities; }
int npcs_base_ents(void) { return base_ents; }
typedef struct { float o[3], yaw, health; uint8_t gone, following, pad[2]; } nsnap_t;
int npcs_snapshot(uint8_t *b, int max)
{
    int pos = 4, n = 0;
    for (int i = 0; i < base_npcs && pos + (int)sizeof(nsnap_t) <= max; i++) {
        npc_t *c = &npcs[i];
        nsnap_t r = {.yaw = c->yaw, .health = c->health, .gone = entities[c->ent].removed || is_dead(c), .following = c->following};
        memcpy(r.o, c->origin, 12);
        memcpy(b + pos, &r, sizeof r); pos += sizeof r; n++;
    }
    b[0] = n; b[1] = n >> 8;
    // characters that came in from the previous map (Barney off the tram): carried records
    int nc = 0;
    for (int i = base_npcs; i < num_npcs && nc < 4 && pos + (int)sizeof(carried_t) <= max; i++) {
        npc_t *c = &npcs[i];
        entity_t *e = &entities[c->ent];
        if (e->removed || is_dead(c) || !c->model) continue;
        carried_t cr;
        memset(&cr, 0, sizeof cr);
        strncpy(cr.model, c->model->name, 31);
        snprintf(cr.kv, sizeof(cr.kv), "\"classname\" \"%s\" \"targetname\" \"%s\" }", e->classname, e->targetname);
        memcpy(cr.rel, c->origin, 12);
        cr.yaw = c->yaw; memcpy(cr.mins, c->mins, 12); memcpy(cr.maxs, c->maxs, 12);
        cr.body = c->body; cr.skin = c->skin; cr.seq = c->idle_seq; cr.flags = c->flags; cr.def = c->def;
        cr.health = c->health; cr.following = c->following; cr.said = c->said;
        memcpy(b + pos, &cr, sizeof cr); pos += sizeof cr; nc++;
    }
    b[2] = nc;
    return pos;
}
void npcs_apply_snapshot(const uint8_t *b, int len)
{
    if (len < 4) return;
    int n = b[0] | (b[1] << 8), pos = 4;
    for (int i = 0; i < n && i < num_npcs && pos + (int)sizeof(nsnap_t) <= len; i++, pos += sizeof(nsnap_t)) {
        nsnap_t r; memcpy(&r, b + pos, sizeof r);
        npc_t *c = &npcs[i];
        if (r.gone) { entities[c->ent].removed = true; c->solid = false; c->health = 0; c->flags |= NF_DEAD | NF_NOTSOLID; continue; }
        memcpy(c->origin, r.o, 12); c->yaw = r.yaw; c->health = r.health; c->following = r.following;
        memcpy(entities[c->ent].origin, r.o, 12);
    }
    int nc = b[2];
    num_carried = 0;
    for (int k = 0; k < nc && k < 16 && pos + (int)sizeof(carried_t) <= len; k++, pos += sizeof(carried_t))
        memcpy(&carried[num_carried++], b + pos, sizeof(carried_t));
    float zero[3] = {0, 0, 0};
    npcs_restore_transition(zero);
}

// auto-aim: the hostile monster nearest the crosshair inside a small cone, in clear sight
bool npc_autoaim(const float *eye, float *dir)
{
    float best = 0.9925f;   // ~7 degrees
    float bestdir[3];
    bool found = false;
    for (int i = 0; i < num_npcs; i++) {
        npc_t *n = &npcs[i];
        if (!npc_alive(i) || !hates(n, 0) || (n->flags & (NF_NOTSOLID | NF_STATIC))) continue;
        float c[3], v[3];
        for (int k = 0; k < 3; k++) c[k] = n->origin[k] + (n->mins[k] + n->maxs[k]) * 0.5f;
        for (int k = 0; k < 3; k++) v[k] = c[k] - eye[k];
        float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l < 1 || l > 2048) continue;
        float dot = (v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2]) / l;
        if (dot <= best) continue;
        gtrace_t tr;
        game_trace(eye, c, NULL, NULL, TR_IGNORE_MONSTERS | TR_IGNORE_PLAYER, -1, &tr);
        if (tr.fraction < 0.98f) continue;
        best = dot;
        for (int k = 0; k < 3; k++) bestdir[k] = v[k] / l;
        found = true;
    }
    if (found) memcpy(dir, bestdir, 12);
    return found;
}
