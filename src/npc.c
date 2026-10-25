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
static const sndset_t S_TURRET = {NULL, 0, "turret/tu_alert.wav", 1, NULL, 0, "turret/tu_die.wav", 1, NULL, 0};
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
    {"monster_apache", "apache", {-160, -160, -80}, {160, 160, 24}, NF_NODROP | NF_FLY, "sk_apache_health", 250, C_MILITARY, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"monster_osprey", "osprey", {-240, -240, -60}, {240, 240, 60}, NF_NODROP | NF_FLY, NULL, 400, C_MILITARY, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"monster_hevsuit_dead", "player", {-16, -16, 0}, {16, 16, 72}, NF_DEAD | NF_NOTSOLID, NULL, 0, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"monster_bigmomma", "big_mom", {-32, -32, 0}, {32, 32, 64}, 0, NULL, 150, C_ALIEN, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"monster_nihilanth", "nihilanth", {-160, -160, 0}, {160, 160, 420}, NF_NODROP | NF_FLY, "sk_nihilanth_health", 800, C_ALIEN, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    // Xen flora (xen.cpp): lights that pull in, hair, trees that swat, spore pods
    {"xen_plantlight", "light", {-8, -8, 0}, {8, 8, 32}, NF_NODROP | NF_STATIC | NF_INVULN | NF_NOTSOLID, NULL, 1, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"xen_hair", "hair", {-4, -4, 0}, {4, 4, 32}, NF_NODROP | NF_STATIC | NF_INVULN | NF_NOTSOLID, NULL, 1, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"xen_tree", "tree", {-30, -30, 0}, {30, 30, 188}, NF_NODROP | NF_STATIC | NF_INVULN, NULL, 1, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"xen_spore_small", "fungus(small)", {-16, -16, 0}, {16, 16, 64}, NF_NODROP | NF_STATIC | NF_INVULN, NULL, 1, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"xen_spore_medium", "fungus", {-40, -40, 0}, {40, 40, 120}, NF_NODROP | NF_STATIC | NF_INVULN, NULL, 1, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"xen_spore_large", "fungus(large)", {-88, -88, 0}, {88, 88, 256}, NF_NODROP | NF_STATIC | NF_INVULN, NULL, 1, C_NONE, 0, NULL, R_NONE, 0, 0, NULL, 0, NULL},
    {"ammo_9mmARclip", "w_9mmarclip", {-16, -16, 0}, {16, 16, 16}, NF_ITEM, NULL, 0, 0, 0, NULL, 0, 0, 0, NULL, 0, NULL},
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

static void barney_gun(npc_t *n, int state);
static bool is_dead(npc_t *n) { return (n->flags & NF_DEAD) || n->ai == AI_DYING || n->ai == AI_DEAD; }
// a corpse whose death animation has finished (or a placed dead body): its pose never changes
bool npc_pose_final(int i)
{
    npc_t *n = &npcs[i];
    return (n->ai == AI_DEAD || (n->flags & NF_DEAD)) && n->sstate == NS_IDLE && n->script < 0;
}
bool npc_alive(int i) { return i >= 0 && i < num_npcs && !is_dead(&npcs[i]) && !(npcs[i].flags & NF_ITEM) && !entities[npcs[i].ent].removed; }
static bool npc_bleeds(npc_t *n);
bool npc_is_item(int i) { return npcs[i].flags & NF_ITEM; }
// a body weapons can hit (TASK_DIE: UTIL_SetSize to a flat box once the death animation ends; the
// placed sitting / hanging / table bodies keep some height)
bool npc_corpse_box(int i, float *mn, float *mx)
{
    npc_t *n = &npcs[i];
    if (!is_dead(n) || (n->flags & NF_ITEM) || entities[n->ent].removed || !npc_bleeds(n)) return false;
    float h = 24;   // a lying body's hitboxes (head, chest) stand about this high; a standing crowbar swing aimed down reaches it
    if (n->model && n->seq >= 0 && n->seq < n->model->numseqs) {
        const char *sn = n->model->seqs[n->seq].name;
        if (strstr(sn, "sit") || strstr(sn, "hang") || strstr(sn, "table") || strstr(sn, "seat")) h = 44;
    }
    mn[0] = fminf(n->mins[0], -16); mn[1] = fminf(n->mins[1], -16); mn[2] = n->mins[2];
    mx[0] = fmaxf(n->maxs[0], 16); mx[1] = fmaxf(n->maxs[1], 16); mx[2] = n->mins[2] + h;
    return true;
}
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
    n->bone_cache = -1;
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
    n->bkind = !strcmp(mname, "nihilanth") ? 1 : !strcmp(mname, "big_mom") ? 2 : !strcmp(mname, "apache") ? 3 : !strcmp(mname, "osprey") ? 4 :
               !strcmp(mname, "light") && !strcmp(cls, "xen_plantlight") ? 5 : !strcmp(mname, "tree") && !strcmp(cls, "xen_tree") ? 6 : 0;
    if (!strcmp(mname, "hgrunt") && !(n->flags & NF_DEAD)) {
        // CHGrunt::Spawn: weapons 0 means MP5 + hand grenades; a shotgun shows in the gun bodygroup
        const char *w = ent_value(e, "weapons");
        n->weapons = w ? atoi(w) : 0;
        if (n->weapons == 0) n->weapons = 1 | 2;
        if ((n->weapons & 8) && m->numbodyparts > 2) n->body += m->bodyparts[2].base;
    }

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
    if (n->bkind == 2) n->health = n->maxhealth = 150 * fmaxf(skill_get("sk_bigmomma_health_factor"), 1);
    if (!strcmp(mname, "hassassin") && !(n->flags & NF_DEAD)) n->weapons = 2;   // CHAssassin throws grenades too

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
                                  "deadback", "deadtable", "sitting2", "sitlookleft",
                                  // bosses
                                  "float", "attack1", "attack1_open", "float_open", "recharge", "die1", "idle1", "mortar",
                                  "spawn", "claw", "angry1", "angry2", "angry3", "breakwall", "falldeath", "walk", "run", "death",
                                  // grunt grenades
                                  "throwgrenade", "launchgrenade"};
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

static int pvs_leaf = -1;   // player leaf whose PVS is in pvs_buf
void npcs_spawn_all(void)
{
    num_npcs = 0;
    pvs_leaf = -1;
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
    // monstermakers: load their monsters' models now (a model loaded mid-map splits the heap)
    for (int i = 1; i < num_entities; i++) {
        entity_t *e = &entities[i];
        if (e->type != ENT_MONSTERMAKER) continue;
        const char *t = ent_value(e, "monstertype");
        int di = t ? def_for_class(t) : -1;
        if (di >= 0) studio_get(defs[di].model);
        else if (t && !strcasecmp(t, "monster_snark")) studio_get("w_squeak");
    }
    // and what grunts and bosses will throw or make during the fight
    for (int i = 0; i < num_npcs; i++) {
        npc_t *n = &npcs[i];
        if (n->weapons & 2) studio_get("w_grenade");
        if (n->weapons & 4) studio_get("grenade");
        if (n->bkind == 2) studio_get("baby_headcrab");
        if (n->bkind == 3) studio_get("rpgrocket");
        if (n->bkind == 4) { studio_get("hgrunt"); studio_get("w_grenade"); }
    }
    debugf("npcs: %d spawned\n", num_npcs);
}

// CMonsterMaker::MakeMonster: a monster appearing during play (alien slaves teleporting in, ...)
int npc_spawn_monster(const char *classname, const float *origin, float yaw, const char *targetname, int maker)
{
    // reuse the slots of a made monster whose body is gone (endless makers: cockroaches)
    int reuse = -1;
    for (int k = 0; k < num_npcs; k++) if (npcs[k].spawned && entities[npcs[k].ent].removed) { reuse = k; break; }
    if (reuse < 0 && (num_entities >= MAX_ENTITIES - 48 || num_npcs >= MAX_NPCS - 8)) return -1;
    int di = def_for_class(classname);
    if (di < 0) return -1;
    // room for it? (the PC maker checks for anything in the box first)
    {
        npc_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        memcpy(tmp.mins, defs[di].mins, 12); memcpy(tmp.maxs, defs[di].maxs, 12);
        gtrace_t tr;
        game_trace(origin, origin, tmp.mins, tmp.maxs, 0, -1, &tr);
        if (tr.startsolid) return -1;
        float pmn[3], pmx[3], po[3];
        pm_get_hull(pmn, pmx); pm_get_origin(po);
        bool overlap = true;
        for (int k = 0; k < 3; k++)
            if (po[k] + pmx[k] <= origin[k] + tmp.mins[k] || po[k] + pmn[k] >= origin[k] + tmp.maxs[k]) overlap = false;
        if (overlap) return -1;
    }
    static char kvpool[32][64];
    static int kvnext;
    char *kv = kvpool[kvnext++ & 31];
    snprintf(kv, 64, "\"classname\" \"%s\" }", classname);
    int idx = reuse >= 0 ? npcs[reuse].ent : num_entities;
    entity_t *e = &entities[idx];
    memset(e, 0, sizeof(*e));
    e->kv = kv;
    strncpy(e->classname, classname, sizeof(e->classname) - 1);
    if (targetname) strncpy(e->targetname, targetname, sizeof(e->targetname) - 1);
    e->type = ENT_OTHER;
    e->model = -1;
    e->renderamt = 255;
    memcpy(e->origin, origin, 12);
    e->angles[1] = yaw;
    if (reuse < 0) num_entities++;
    int saved = num_npcs;
    if (reuse >= 0) num_npcs = reuse;   // new_npc() fills npcs[num_npcs]
    int before = num_npcs;
    spawn_one(idx, NULL);
    bool made = num_npcs > before;
    if (reuse >= 0) num_npcs = saved;
    if (!made) { e->removed = true; return -1; }
    npc_t *n = &npcs[reuse >= 0 ? reuse : num_npcs - 1];
    n->spawned = true;
    n->maker = maker;
    if (maker > 0) entities[maker].maker_live++;
    n->next_look = 0;   // looks around straight away
    return (int)(n - npcs);
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
    // step up 18, across, then down (no room above: across from where it stands)
    float up[3] = {n->origin[0], n->origin[1], n->origin[2] + 18};
    float fw[3] = {up[0] + dx, up[1] + dy, up[2]};
    gtrace_t t1;
    game_trace(up, fw, n->mins, n->maxs, 0, self, &t1);
    if (t1.startsolid) {
        up[2] = fw[2] = n->origin[2];
        game_trace(up, fw, n->mins, n->maxs, 0, self, &t1);
        if (t1.startsolid) return false;
    }
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
    float d = dist2d(a, b);
    if (d > 48) {
        // quick reject: a wall across the line near the top of the hull blocks the walk too
        // (one point trace instead of a hull walk in 24 unit steps); high up, stairs don't hit it
        float hz = n->maxs[2] - 4;
        float la[3] = {a[0], a[1], a[2] + hz}, lb[3] = {b[0], b[1], b[2] + hz};
        gtrace_t tr;
        game_trace(la, lb, NULL, NULL, TR_IGNORE_PLAYER | TR_IGNORE_MONSTERS | TR_IGNORE_GLASS, c->self, &tr);
        if (tr.fraction < 1 && !tr.startsolid && fabsf(tr.normal[2]) < 0.7f) return false;
    }
    float save[3];
    memcpy(save, n->origin, 12);
    bool save_g = n->onground;
    memcpy(n->origin, a, 12);
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
        // on the world (or not known yet): look again now and then, in case it was really a
        // mover that has since gone (c1a1's falling lift)
        if (n->ground_ent <= 0 || n->ground_ent >= num_entities) {
            float now = game_time();
            if (now < n->ground_check) return;
            n->ground_check = now + 0.25f;
        } else if (entities[n->ground_ent].removed || !entities[n->ground_ent].solid) {
            n->ground_ent = -1;   // the floor broke or went away (c1a1's lift floor gibs): fall
        } else {
            entity_t *g = &entities[n->ground_ent];
            if (g->origin[0] == n->ground_prev[0] && g->origin[1] == n->ground_prev[1] && g->origin[2] == n->ground_prev[2]) return;
        }
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
    if (tr.startsolid) { n->onground = true; n->vz = 0; if (tr.ent > 0) { n->ground_ent = tr.ent; } }
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
    static float next;
    if (now < next && now >= next - 1) return;   // a few times a second is plenty (and survives map changes)
    next = now + 0.2f;
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

static int npc_leaf(npc_t *n)
{
    if (n->leaf_cache > 0 && n->leaf_org[0] == n->origin[0] && n->leaf_org[1] == n->origin[1] && n->leaf_org[2] == n->origin[2]) return n->leaf_cache;
    float c[3];
    npc_center(n, c);
    int l = bsp_leaf_for_point(c);
    memcpy(n->leaf_org, n->origin, 12);
    n->leaf_cache = l;
    return l;
}

static bool player_in_pvs(npc_t *n)
{
    if (!pvs_ok) return true;
    int l = npc_leaf(n);
    if (l <= 0) return true;
    return pvs_buf[(l - 1) >> 3] & (1 << ((l - 1) & 7));
}

// floor light under the monster: only when it has moved or every so often (light styles)
static void npc_relight(npc_t *n, float now)
{
    float d = fabsf(n->origin[0] - n->light_pos[0]) + fabsf(n->origin[1] - n->light_pos[1]) + fabsf(n->origin[2] - n->light_pos[2]);
    if (d < 2 && now < n->light_time) return;
    memcpy(n->light_pos, n->origin, 12);
    n->light_time = now + 0.5f + (rand() % 100) / 400.0f;
    bsp_light_at(n->origin, n->light);
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
    bool fov = n->ai == AI_IDLE && !(n->flags & NF_STATIC) && game_time() >= n->alert_until;
    if (hates(n, 0) && player_in_pvs(n)) {
        float d = dist3(n->origin, player_pos);
        if (d < 2048 && can_see(n, 0, fov)) { best = 0; bd = d; }
    }
    // other monsters: only for allies, soldiers and machines (aliens go for humans too).
    // Sight traces are the cost: test the candidates nearest first and stop at the first one seen,
    // at most a handful per look (the rest get their turn at the next look)
    int cand[24]; float cd[24]; int nc = 0;
    for (int i = 0; i < num_npcs; i++) {
        npc_t *t = &npcs[i];
        if (t == n || !npc_alive(i)) continue;
        float d = dist3(n->origin, t->origin);
        if (d >= bd || d > 1500) continue;
        if (!hates(n, i + 1)) continue;
        if (nc == 24 && d >= cd[23]) continue;   // full: keep the 24 nearest
        int k = nc < 24 ? nc++ : 23;
        while (k > 0 && cd[k - 1] > d) { cand[k] = cand[k - 1]; cd[k] = cd[k - 1]; k--; }
        cand[k] = i; cd[k] = d;
    }
    // the looker's PVS rules out most candidates without a trace
    const uint8_t *pvs = NULL;
    if (nc) { int ll = npc_leaf(n); if (ll > 0) pvs = bsp_leaf_pvs(ll); }
    for (int k = 0, tries = 0; k < nc && tries < 6; k++) {
        if (pvs) { int tl = npc_leaf(&npcs[cand[k]]); if (tl > 0 && !(pvs[(tl - 1) >> 3] & (1 << ((tl - 1) & 7)))) continue; }
        tries++;
        if (can_see(n, cand[k] + 1, fov)) { best = cand[k] + 1; break; }
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
            // a noise only makes it alert (CBaseMonster: bits_COND_HEAR_SOUND -> SCHED_ALERT_FACE /
            // SCHED_INVESTIGATE_SOUND); it fights only once it actually sees the player
            if (source == 0 && hates(n, 0)) {
                if (game_time() >= n->alert_until) n->investigate = game_time() + 0.6f + (rand() % 100) / 100.0f;
                memcpy(n->heard, origin, 12);
                n->alert_until = game_time() + 8;
            }
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
    if (n->maker) { extern void ents_maker_child_died(int maker); ents_maker_child_died(n->maker); n->maker = 0; }
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
static bool nihilanth_damage(npc_t *n, float dmg, int attacker);
static bool heli_damage(npc_t *n, float dmg, int dmgtype);
static bool osprey_damage(npc_t *n, float dmg, int dmgtype);
static void bigmomma_damage(npc_t *n, float *dmg, int dmgtype);
static void grunt_take_cover(npc_t *n);
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
        if (ne->removed || (n->flags & NF_ITEM) || !npc_bleeds(n)) return;
        // CBaseMonster::DeadTakeDamage: only DMG_GIB_CORPSE damage (crowbar, explosions, crushing,
        // falls, sonic) destroys a body: at once if the hit is at least what is left of it, otherwise
        // a tenth of it wears the body down. Bullets only draw blood.
        if (n->corpse_hp <= 0) n->corpse_hp = strstr(ne->classname, "_dead") ? 8 : fmaxf(n->maxhealth * 0.5f, 1);
        if (dmgtype & (DMG_CRUSH | DMG_FALL | DMG_BLAST | DMG_SONIC | DMG_CLUB | DMG_ALWAYSGIB_N64)) {
            if (n->corpse_hp <= dmg) npc_gib(n);
            else n->corpse_hp -= dmg * 0.1f;
        }
        return;
    }
    if (!npc_alive(i) || (n->flags & NF_INVULN)) return;
    if (n->bkind == 1) { nihilanth_damage(n, dmg, attacker); return; }
    if (n->bkind == 3) { heli_damage(n, dmg, dmgtype); return; }
    if (n->bkind == 4) { osprey_damage(n, dmg, dmgtype); return; }
    if (n->bkind == 2) bigmomma_damage(n, &dmg, dmgtype);
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
    if (n->weapons && n->ai == AI_COMBAT && dmg >= 8 && rand() % 2 == 0) grunt_take_cover(n);
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
    if (n->maker) { extern void ents_maker_child_died(int maker); ents_maker_child_died(n->maker); n->maker = 0; }
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
        if (!strcmp(d->model, "barney")) {
            snd_emit(n->ent, CHAN_WEAPON, "barney/ba_attack2.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
            barney_gun(n, 1);   // BARNEY_AE_DRAW: the pistol in his hand
        }
        else if (grunt && (n->weapons & 8)) {   // CHGrunt::Shotgun: a spread of buckshot
            snd_emit(n->ent, CHAN_WEAPON, "weapons/sbarrel1.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
            int pel = (int)skill_get("sk_hgrunt_pellets");
            fire_bullet_at(n, t, 5, 0.13f, pel > 0 ? pel : 5);
            npc_hear(n->origin, 600, (n - npcs) + 1);
            break;
        }
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
        {   // two TE_BEAMCYLINDERs (shockwave.spr, width 16): to the full radius and to half of it,
            // coloured by how many houndeyes are in the pack (WriteBeamColor)
            int pack = 0;
            for (int i = 0; i < num_npcs; i++)
                if (npc_alive(i) && D(&npcs[i]) && !strcmp(D(&npcs[i])->model, "houndeye") && dist3(npcs[i].origin, n->origin) < 384) pack++;
            static const uint8_t col[4][3] = {{188, 220, 255}, {101, 133, 221}, {67, 85, 255}, {62, 33, 211}};
            const uint8_t *cc = col[pack <= 1 ? 0 : pack >= 4 ? 3 : pack - 1];
            extern void game_effect_shockring(const float *o, float maxr, float height, float life, int r, int g, int b);
            float base[3] = {n->origin[0], n->origin[1], n->origin[2] + 8};
            game_effect_shockring(base, 384, 16, 0.3f, cc[0], cc[1], cc[2]);
            game_effect_shockring(base, 192, 16, 0.3f, cc[0], cc[1], cc[2]);
        }
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
    case R_FLAME: {
        // CGargantua::FlameDamage: two jets of flame from the eyes
        extern void game_effect_beam_plain(const float *, const float *, int, int, int);
        float a = n->yaw * (float)M_PI / 180, side[3] = {-sinf(a) * 14, cosf(a) * 14, 0};
        for (int k = -1; k <= 1; k += 2) {
            float from[3] = {eye[0] + side[0] * k, eye[1] + side[1] * k, eye[2]};
            game_effect_beam_plain(from, tp, 255, 120 + rand() % 60, 30);
        }
        game_effect_particles(tp, 4, 120, 255, 160, 60, 0.4f, -200);
        if (game_time() > n->bt[3]) { n->bt[3] = game_time() + 1.5f; snd_emit(n->ent, CHAN_WEAPON, "garg/gar_flamerun1.wav", 1, ATTN_NORM, PITCH_NORM, NULL); }
        if (dist < d->rmax) damage_target(n, t, dmg * 3, DMG_BURN);
        break;
    }
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
    float rmax = (n->weapons & 8) ? 640 : d->rmax;   // shotgun grunts close in
    if (d->ranged != R_NONE && dist >= d->rmin && dist <= rmax) {
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
    if (!(n->said & 1)) {
        if (pd < 256 && player_alive() && ok_to_speak(n) && can_see(n, 0, false)) {
            const char *g = (n->flags & NF_PREDIS) ? grp(n, "SC_PHELLO", NULL) : grp(n, "SC_HELLO", "BA_HELLO");
            if (g) npc_say(n, g, 3.2f, 1, ATTN_IDLE);
            n->said |= 1;
            n->talk_target = 0;
        }
        return;
    }
    if (now < n->idle_speak_time) return;
    n->idle_speak_time = now + 3 + (rand() % 100) / 25.0f;
    bool see_player = pd < 512 && player_alive() && can_see(n, 0, false);   // only when about to speak
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
    // alerted by a noise: look that way, then walk over to where it came from (SCHED_INVESTIGATE_SOUND)
    if (now < n->alert_until && !n->following && !(n->flags & (NF_STATIC | NF_SCI)) && klass(n) != C_ALLY) {
        if (now < n->investigate) { face_yaw(n, yaw_to(n->origin, n->heard), 180, dt); return; }
        float g[3] = {n->heard[0], n->heard[1], n->heard[2] - 36};
        if (dist3(n->origin, g) < 128) { if (n->npath) { n->npath = 0; set_seq(n, n->idle_seq); } n->alert_until = now + 2; return; }
        if (n->npath == 0 || dist3(n->goal, g) > 128 || n->stuck_time > 1.0f) start_moving(n, g, false);
        if (follow_route(n, n->speed, dt, 96)) { n->npath = 0; set_seq(n, n->idle_seq); }
        return;
    } else if (n->alert_until > 0 && now >= n->alert_until) {
        n->alert_until = 0;
        if (n->npath && !n->following) { n->npath = 0; set_seq(n, n->idle_seq); }
    }
    (void)i;
}

static void lob_velocity(const float *from, const float *to, float peak_above, float g, float *vel);
int proj_spawn_vel(int kind, const float *origin, const float *vel, int owner, int target);
// CHGrunt: a grenade (hand thrown or from the launcher) at an enemy that is hiding or bunched up
static bool grunt_grenade(npc_t *n, int t, bool vis, float dist)
{
    float now = game_time();
    if (!(n->weapons & (2 | 4)) || now < n->next_grenade || now < n->bt[1]) return false;
    n->bt[1] = now + 1.0f;   // decide once a second
    float d_ls = dist3(n->origin, n->last_seen);
    bool launcher = (n->weapons & 4) != 0;
    if (d_ls < 220 || d_ls > (launcher ? 1200 : 800)) return false;
    bool hiding = !vis && now - n->bt[0] < 5;
    if (!hiding && !(vis && rand() % 4 == 0)) return false;
    float src[3], vel[3];
    npc_eye(n, src);
    float to[3] = {n->last_seen[0], n->last_seen[1], n->last_seen[2] - (t == 0 ? 30 : 0)};
    if (launcher) lob_velocity(src, to, 96, 400, vel);
    else lob_velocity(src, to, 128, 800, vel);
    // the arc must not hit the ceiling right away (VecCheckToss)
    float peak[3] = {src[0] + vel[0] * 0.3f, src[1] + vel[1] * 0.3f, src[2] + vel[2] * 0.3f};
    if (!game_visible(src, peak)) return false;
    proj_spawn_vel(launcher ? 2 /* P_ARGRENADE */ : 1 /* P_HGRENADE */, src, vel, (n - npcs) + 1, -1);
#ifdef BOSSDEBUG
    debugf("grunt %d grenade (%s) at %.0f %.0f %.0f %s\n", (int)(n - npcs), launcher ? "launcher" : "hand", to[0], to[1], to[2], hiding ? "hiding" : "visible");
#endif
    if (launcher) snd_emit(n->ent, CHAN_WEAPON, "weapons/glauncher.wav", 0.8f, ATTN_NORM, PITCH_NORM, NULL);
    if (!strcmp(D(n)->model, "hgrunt") && ok_to_speak(n)) npc_say(n, launcher ? "HG_THROW" : "HG_GREN", 2, 1, ATTN_NORM);
    if (!set_activity(n, ACT_RANGE_ATTACK2)) set_activity(n, ACT_RANGE_ATTACK1);
    restart_seq(n, n->seq);
    n->ai = AI_ATTACK;
    n->attack_kind = 3;
    n->attack_hit_done = true;
    n->npath = 0;
    n->next_attack = now + 1.0f;
    n->next_grenade = now + 6 + rand() % 4;
    (void)dist;
    return true;
}
// CHGrunt: hurt in a fight -> run for a spot the enemy can't see (bits_COND_HEAVY_DAMAGE -> TAKE_COVER)
static void grunt_take_cover(npc_t *n)
{
    float now = game_time();
    if (n->enemy < 0 || now < n->cover_until + 2 || n->script >= 0) return;
    float ep[3];
    target_pos(n->enemy, ep);
    ep[2] += n->enemy == 0 ? 28 : 0;
    // nearest few candidate nodes first, then at most 10 sight traces
    int cand[10], nc = 0;
    float cd[10];
    for (int k = 0; k < nav_num_nodes(); k++) {
        const float *o = nav_node_origin(k);
        float dn = dist3(o, n->origin);
        if (dn > 640 || dn < 96 || fabsf(o[2] - n->origin[2]) > 96) continue;
        if (nc == 10 && dn >= cd[9]) continue;
        int j = nc < 10 ? nc++ : 9;
        while (j > 0 && cd[j - 1] > dn) { cd[j] = cd[j - 1]; cand[j] = cand[j - 1]; j--; }
        cd[j] = dn; cand[j] = k;
    }
    int best = -1;
    for (int c = 0; c < nc && best < 0; c++) {
        const float *o = nav_node_origin(cand[c]);
        float eye[3] = {o[0], o[1], o[2] + 40};
        if (!game_visible(eye, ep)) best = cand[c];
    }
    if (best < 0) return;
    start_moving(n, nav_node_origin(best), true);
    n->cover_until = now + 4;
#ifdef BOSSDEBUG
    debugf("grunt %d takes cover at node %d\n", (int)(n - npcs), best);
#endif
    if (!strcmp(D(n)->model, "hgrunt") && ok_to_speak(n)) npc_say(n, "HG_COVER", 2, 1, ATTN_NORM);
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
    // line of sight to the enemy: a trace a few times a second, not every frame
    if (now >= n->vis_time || n->vis_target != t) {
        n->vis_cache = can_see(n, t, false);
        n->vis_time = now + 0.12f + (rand() % 100) / 1000.0f;
        n->vis_target = t;
    }
    bool vis = n->vis_cache;
    if (vis) memcpy(n->last_seen, tp, 12);
    float dist = target_dist(n, t);
    // scientists run from danger
    if (klass(n) == C_ALLY && (n->flags & NF_SCI)) { n->ai = AI_FLEE; n->fear_time = now; n->next_plan = n->flee_look = 0; return; }
    if (n->weapons) {   // human grunts
        if (vis) n->bt[0] = now;
        if (now < n->cover_until && n->npath > 0) {   // running for cover
            if (!set_activity(n, ACT_RUN)) set_activity(n, ACT_WALK);
            if (follow_route(n, n->speed, dt, 16)) { n->npath = 0; set_activity(n, ACT_IDLE); }
            return;
        }
        if (grunt_grenade(n, t, vis, dist)) return;
    }
    // face the enemy while attacking / standing
    if (try_attack(n, dist, vis)) {
        n->yaw = yaw_to(n->origin, tp);
        return;
    }
    if (n->flags & NF_STATIC) { face_yaw(n, yaw_to(n->origin, tp), 180, dt); return; }
    // chase (or keep distance for ranged attackers that can see us)
    bool ranged_ok = d && d->ranged != R_NONE && d->ranged != R_LEAP && vis && dist <= ((n->weapons & 8) ? 640 : d->rmax) && dist >= d->rmin;
    if (ranged_ok) {
        face_yaw(n, yaw_to(n->origin, tp), 240, dt);
        if (!set_activity(n, ACT_COMBAT_IDLE) && !set_activity(n, ACT_IDLE_ANGRY)) set_activity(n, ACT_IDLE);
        n->npath = 0;
        return;
    }
    float stop = d && d->melee_dmg ? d->melee_range * 0.8f : 48;
    if (dist <= stop && vis) { face_yaw(n, yaw_to(n->origin, tp), 300, dt); set_activity(n, ACT_IDLE); return; }
    if ((n->npath == 0 || dist3(n->goal, n->last_seen) > 96 || n->stuck_time > 1.0f || n->pathi >= n->npath) && now >= n->next_plan) {
        float g[3] = {n->last_seen[0], n->last_seen[1], n->last_seen[2] - (t == 0 ? 36 : 0)};
        start_moving(n, g, true);
        // route searches are costly: a few a second, and a failed one waits longer before retrying
        n->next_plan = now + (n->npath > 0 ? 0.25f : 0.6f) + (rand() % 100) / 1000.0f;
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
        // sight checks and route searches are the costly part: a few times a second is plenty
        if (now >= n->flee_look) {
            n->look_hidden = dist3(n->origin, tp) > 600 && !can_see(n, n->enemy, false);
            n->flee_look = now + 0.3f + (rand() % 100) / 1000.0f;
        }
        if (n->look_hidden) { if (!set_activity(n, ACT_COWER)) set_activity(n, ACT_IDLE); return; }
        if ((n->npath == 0 || n->pathi >= n->npath || n->stuck_time > 1) && now >= n->next_plan) {
            n->next_plan = now + 0.5f + (rand() % 200) / 1000.0f;
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
    if (n->tongue_len <= 0) {   // the floor under it (traced once: barnacles never move)
        gtrace_t tr;
        float st[3] = {n->origin[0], n->origin[1], n->origin[2] - 2};
        float end[3] = {n->origin[0], n->origin[1], n->origin[2] - 2048};
        game_trace(st, end, NULL, NULL, TR_IGNORE_MONSTERS | TR_IGNORE_PLAYER, n - npcs, &tr);
        n->tongue_len = fmaxf(fabsf(n->origin[2] - tr.endpos[2]), 1);
        n->alt = n->tongue_len;                 // already hanging down
    }
    float len = n->tongue_len;
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

// ---------------------------------------------------------------- bosses
// nihilanth.cpp, bigmomma.cpp, apache.cpp, osprey.cpp: the PC behaviour in a simpler form
int proj_spawn_vel(int kind, const float *origin, const float *vel, int owner, int target);
void game_spawn_gibs(const float *o, int count, bool alien, float speed);
static float key_float(int ent, const char *key, float def) { const char *v = ent_value(&entities[ent], key); return v ? atof(v) : def; }
static int find_named(const char *name) { return name && name[0] ? ents_find_targetname(name, 1) : -1; }
static bool play_named(npc_t *n, const char *name, bool restart)
{
    int s = studio_find_seq(n->model, name);
    if (s < 0) return false;
    if (restart || s != n->seq) restart_seq(n, s);
    return true;
}
// a lob that lands on `to` (VecCheckToss with a fixed peak above both ends)
static void lob_velocity(const float *from, const float *to, float peak_above, float g, float *vel)
{
    float peak = fmaxf(from[2], to[2]) + peak_above;
    float up = sqrtf(2 * g * fmaxf(peak - from[2], 1)), t1 = up / g, t2 = sqrtf(2 * fmaxf(peak - to[2], 1) / g);
    float T = t1 + t2;
    vel[0] = (to[0] - from[0]) / T; vel[1] = (to[1] - from[1]) / T; vel[2] = up;
}
static void boss_explode(npc_t *n, int count)
{
    for (int k = 0; k < count; k++) {
        float o[3] = {n->origin[0] + (rand() % 200 - 100), n->origin[1] + (rand() % 200 - 100), n->origin[2] + (rand() % 100)};
        game_effect_explosion(o, 180);
    }
    snd_emit(-1, CHAN_AUTO, "weapons/mortarhit.wav", 1, 0.3f, PITCH_NORM, n->origin);
    float c[3];
    npc_center(n, c);
    game_spawn_gibs(c, 10, false, 400);
}

// ---- Nihilanth: floats in his column (n_min..n_max) at the player's height, throws zap balls and,
// every third volley, a teleport ball that sends the player to the next arena (n_teleport1..4).
// The recharger crystals (n_recharger1..3) restore him; with them gone his health runs out, the
// head opens, and only shots from above (into the brain) can finish him.
static int nih_index = -1;
void npc_teleball_hit(void)
{
    if (nih_index < 0 || nih_index >= num_npcs) return;
    npc_t *n = &npcs[nih_index];
    extern bool ents_teleport_player(const char *name);
    for (; n->bi[1] <= 4; n->bi[1]++) {
        char tn[24], ln[24];
        snprintf(tn, sizeof(tn), "n_teleport%d", n->bi[1]);
        snprintf(ln, sizeof(ln), "n_leaving%d", n->bi[1]);
        if (ents_teleport_player(tn)) { ents_fire_targets(ln); n->bi[1]++; return; }
    }
}
static int nih_rechargers(int *which)
{
    int c = 0;
    for (int k = 1; k <= 3; k++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "n_recharger%d", k);
        int e = find_named(nm);
        if (e > 0 && !entities[e].removed) { c++; if (which) *which = k; }
    }
    return c;
}
static void nihilanth_think(npc_t *n, float dt)
{
    float now = game_time();
    nih_index = n - npcs;
    bool open = n->bi[2] >= 2;
    if (n->bstate == 0) {   // asleep until the map wakes him (trigger_auto "nihilanth")
        play_named(n, "float", false);
        n->bt[3] += dt;
        n->origin[2] += sinf(n->bt[3]) * 6 * dt;
        return;
    }
    if (n->bstate == 3) {
        // CNihilanth::DyingThink: he rises to the top of his column, and once there fires n_dead.
        // The map's end sequence (end_mm, end2_mm: beams, explosions, flashes) runs from that, and
        // its n_end_relay uses him at the end, which sends the player through n_ending (nihilanth.cpp
        // CommandUse: the m_szDeadTouch trigger touched by the player)
        float top = n->bi[0] ? n->bt[2] - 420 : n->origin[2];
        float dz = top - n->origin[2];
        n->origin[2] += fmaxf(fminf(dz, 120 * dt), -120 * dt);
        if (n->bi[3] == 0 && ((fabsf(dz) < 16 && now > n->bt[0]) || now > n->bt[0] + 8)) {
            n->bi[3] = 1;
            boss_explode(n, 4);
            ents_fire_targets("n_dead");
        }
        return;
    }
    if (!n->bi[0]) {
        int mn = find_named("n_min"), mx = find_named("n_max");
        n->bt[1] = mn > 0 ? entities[mn].origin[2] : n->origin[2] - 200;
        n->bt[2] = mx > 0 ? entities[mx].origin[2] : n->origin[2] + 2000;
        n->bi[0] = 1; n->bi[1] = 1;
        n->next_attack = now + 4;
    }
    // follow the player's height (his eyes level with the player), turn to face them
    float want = fminf(fmaxf(player_pos[2] - 220, n->bt[1]), n->bt[2] - 420);
    float dz = want - n->origin[2];
    n->origin[2] += fmaxf(fminf(dz, 80 * dt), -80 * dt) + sinf(now * 1.3f) * 10 * dt;
    face_yaw(n, yaw_to(n->origin, player_pos), 25, dt);
    // regain strength from a crystal (n_draw beams) when hurt
    int which = 0;
    int rech = nih_rechargers(&which);
    if (n->health < n->maxhealth * 0.5f && rech > 0 && now > n->bt[0]) {
        char dn[16];
        snprintf(dn, sizeof(dn), "n_draw%d", which);
        ents_fire_targets(dn);
        play_named(n, "recharge", true);
        snd_emit(n->ent, CHAN_VOICE, "x/x_recharge1.wav", 1, 0.2f, PITCH_NORM, NULL);
        n->health = fminf(n->maxhealth, n->health + n->maxhealth * 0.5f);
        n->bt[0] = now + 15;
    }
    if (now >= n->next_attack && player_alive()) {
        play_named(n, open ? "attack1_open" : "attack1", true);
        float src[3] = {n->origin[0], n->origin[1], n->origin[2] + 300};
        float dir[3] = {player_pos[0] - src[0], player_pos[1] - src[1], player_pos[2] - src[2]};
        float l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (int k = 0; k < 3; k++) dir[k] /= l > 1 ? l : 1;
        bool tele = (++n->bi[3] % 3 == 0) && n->bi[1] <= 4;
        if (tele) {
            proj_spawn(14 /* P_TELEBALL */, src, dir, (n - npcs) + 1, 0);
            snd_emit(n->ent, CHAN_WEAPON, "x/x_teleattack1.wav", 1, 0.2f, PITCH_NORM, NULL);
        } else {
            for (int b = 0; b < 2 + (open ? 1 : 0); b++) {
                float s2[3] = {src[0] + (b - 1) * 120.0f * cosf((n->yaw + 90) * (float)M_PI / 180), src[1] + (b - 1) * 120.0f * sinf((n->yaw + 90) * (float)M_PI / 180), src[2]};
                proj_spawn(15 /* P_NIHZAP */, s2, dir, (n - npcs) + 1, 0);
            }
            snd_emit(n->ent, CHAN_WEAPON, "x/x_ballattack1.wav", 1, 0.2f, PITCH_NORM, NULL);
        }
        n->next_attack = now + 3.0f + (rand() % 100) / 50.0f;
    }
    if (n->seq_done) play_named(n, open ? "float_open" : "float", false);
}
static bool nihilanth_damage(npc_t *n, float dmg, int attacker)
{
    if (n->bstate == 0) n->bstate = 1;
    if (n->bstate == 3) return true;
    float now = game_time();
    if (n->bi[2] < 2) {
        if (n->health - dmg < 1) {
            n->health = 1;
            if (nih_rechargers(NULL) == 0) {   // nothing left to heal him: the head opens
                n->bi[2] = 2;
                play_named(n, "float_open", true);
                snd_emit(n->ent, CHAN_VOICE, "x/x_pain2.wav", 1, 0.2f, PITCH_NORM, NULL);
            }
        } else n->health -= dmg;
    } else {
        // the PC only counts hits on the brain: from up level with or above his open skull
        if (attacker == 0 && player_pos[2] < n->origin[2] + 260) return true;
        n->health -= dmg;
        if (n->health <= 0) {
            n->bstate = 3;
            n->solid = false;
            play_named(n, "die1", true);
            snd_emit(n->ent, CHAN_VOICE, "x/x_die1.wav", 1, 0.1f, PITCH_NORM, NULL);
            n->bt[0] = now + studio_seq_duration(n->model, n->seq) + 0.5f;
            n->bi[3] = 0;
            return true;
        }
    }
    if (now > n->pain_time) { n->pain_time = now + 2; snd_emit(n->ent, CHAN_VOICE, rand() & 1 ? "x/x_pain1.wav" : "x/x_pain2.wav", 1, 0.2f, PITCH_NORM, NULL); }
    return true;
}

// ---- Big Momma (the Gonarch): walks a chain of info_bigmomma nodes from "netname". At a node
// she takes its health, fires its reachtarget, plays its reachsequence and waits reachdelay;
// with no health at a node she moves straight on, otherwise she stays and fights until hurt
// enough. Mortar lobs, baby headcrabs and claws; she can only die once the path is finished.
static void bm_node_start(npc_t *n, const char *name)
{
    int t = find_named(name);
    if (t <= 0 || strcasecmp(entities[t].classname, "info_bigmomma")) { n->bstate = 3; n->npath = 0; return; }
    n->bi[0] = t;
    n->bstate = 1;
    bool run = ((int)key_float(t, "spawnflags", 0) & 1) || (entities[t].spawnflags & 1);
    n->running = run;
    n->speed = move_speed(n, run);
    start_moving(n, entities[t].origin, run);
    set_activity(n, run ? ACT_RUN : ACT_WALK);
    const char *pre = ent_value(&entities[t], "presequence");
    n->bt[0] = 0;
    if (pre && play_named(n, pre, true)) n->bt[0] = game_time() + studio_seq_duration(n->model, n->seq);
    n->stuck_count = 0;
}
static void bm_node_reach(npc_t *n)
{
    int t = n->bi[0];
    float now = game_time();
    float h = key_float(t, "health", 0);
    if (h > 0) n->health = n->maxhealth = h * fmaxf(skill_get("sk_bigmomma_health_factor"), 1);
    const char *rt = ent_value(&entities[t], "reachtarget");
    if (rt) ents_fire_targets(rt);
    const char *rs = ent_value(&entities[t], "reachsequence");
    float wait = key_float(t, "reachdelay", 0);
    if (rs && play_named(n, rs, true)) wait += studio_seq_duration(n->model, n->seq);
    else set_seq(n, n->idle_seq);
    n->bt[0] = now + wait;
    n->bstate = 2;
    n->npath = 0;
    n->bi[1] = h == 0 ? 1 : 0;   // advance straight on when the node has no health
}
static void bm_fight(npc_t *n, float dt)
{
    float now = game_time();
    if (!player_alive()) return;
    bool vis = can_see(n, 0, false);
    face_yaw(n, yaw_to(n->origin, player_pos), 90, dt);
    if (n->attack_hit_time > 0 && now >= n->attack_hit_time) {
        n->attack_hit_time = 0;
        if (dist3(n->origin, player_pos) < 190) damage_target(n, 0, skill_get("sk_bigmomma_dmg_slash"), DMG_SLASH);
    }
    if (now < n->next_attack || !vis) return;
    float d = dist3(n->origin, player_pos);
    if (d < 160) {
        play_named(n, "claw", true);
        n->attack_hit_time = now + 0.4f;
        n->next_attack = now + 1.5f;
        snd_emit(n->ent, CHAN_WEAPON, "gonarch/gon_attack1.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
    } else if (n->bi[2] < 20 && now >= n->bt[2] && d < 800) {
        // CBigMomma::LayHeadcrab
        play_named(n, "spawn", true);
        float a = n->yaw * (float)M_PI / 180;
        float o[3] = {n->origin[0] + cosf(a) * 64, n->origin[1] + sinf(a) * 64, n->origin[2] + 16};
        int c = npc_spawn_monster("monster_babycrab", o, n->yaw, "", 0);
        if (c >= 0) n->bi[2]++;
        snd_emit(n->ent, CHAN_WEAPON, "gonarch/gon_birth1.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        n->bt[2] = now + 8 + rand() % 5;
        n->next_attack = now + 2;
    } else if (now >= n->bt[1] && d < 1600) {
        // CBigMomma::LaunchMortar: an acid lob at the player
        play_named(n, "mortar", true);
        float src[3] = {n->origin[0], n->origin[1], n->origin[2] + 150}, vel[3];
        float to[3] = {player_pos[0], player_pos[1], player_pos[2] - 36};
        lob_velocity(src, to, 200, 800, vel);
        proj_spawn_vel(13 /* P_MORTAR */, src, vel, (n - npcs) + 1, -1);
        snd_emit(n->ent, CHAN_WEAPON, "gonarch/gon_sack1.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        n->bt[1] = now + 4 + rand() % 3;
        n->next_attack = now + 1.5f;
    }
}
static void bigmomma_think(npc_t *n, float dt)
{
    float now = game_time();
    if (n->bstate == 0) {
        const char *nn = ent_value(&entities[n->ent], "netname");
        n->bt[1] = now + 3; n->bt[2] = now + 6;
        bm_node_start(n, nn);
        return;
    }
    if (n->bstate == 1) {   // walking to the node
        if (now < n->bt[0]) return;   // its presequence first
        if (n->seq_done || (n->seq != studio_find_activity(n->model, n->running ? ACT_RUN : ACT_WALK))) set_activity(n, n->running ? ACT_RUN : ACT_WALK);
        float radius = key_float(n->bi[0], "radius", 64);
        bool arrived = follow_route(n, n->speed, dt, radius) || dist2d(n->origin, entities[n->bi[0]].origin) < radius;
        if (!arrived && n->stuck_time > 1.5f) {
            n->stuck_time = 0;
            if (++n->stuck_count >= 3) { memcpy(n->origin, entities[n->bi[0]].origin, 12); arrived = true; }
            else route_to(n, entities[n->bi[0]].origin);
        }
        if (arrived) bm_node_reach(n);
        gravity(n, dt);
        npc_relight(n, now);
        return;
    }
    if (n->bstate == 2) {   // at a node
        if ((n->bi[1] & 1) && now >= n->bt[0]) {
            const char *next = ent_value(&entities[n->bi[0]], "target");
            bm_node_start(n, next);
            return;
        }
        if (now >= n->bt[0]) { bm_fight(n, dt); if (n->seq_done) set_seq(n, n->idle_seq); }
        gravity(n, dt);
        return;
    }
    // path finished: fight it out
    bm_fight(n, dt);
    if (n->seq_done) set_seq(n, n->idle_seq);
    gravity(n, dt);
}
static void bigmomma_damage(npc_t *n, float *dmg, int dmgtype)
{
    if (dmgtype & DMG_ACID) { *dmg = 0; return; }   // her own mortar
    if (n->bstate != 3 && n->health <= *dmg) {
        // not before the end of her path: reaching zero sends her to the next node instead
        n->health = *dmg + 1;
        n->bi[1] |= 1;
        if (n->bstate == 2) n->bt[0] = fminf(n->bt[0], game_time());
    }
}

// ---- Apache: waits for a trigger (spawnflags 4/64), then flies its path_corner chain; when it
// sees the player it closes in, strafes with the chaingun and fires rockets. Armoured against
// bullets; going down it spins, falls and blows up.
static void heli_move(npc_t *n, const float *goal, float maxspeed, float accel, float dt)
{
    float d[3] = {goal[0] - n->origin[0], goal[1] - n->origin[1], goal[2] - n->origin[2]};
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float want = fminf(maxspeed, l * 1.5f);
    for (int k = 0; k < 3; k++) {
        float tv = l > 1 ? d[k] / l * want : 0;
        float dv = tv - n->bv[k], mx = accel * dt;
        n->bv[k] += fmaxf(fminf(dv, mx), -mx);
    }
    float to[3] = {n->origin[0] + n->bv[0] * dt, n->origin[1] + n->bv[1] * dt, n->origin[2] + n->bv[2] * dt};
    static const float smn[3] = {-48, -48, -48}, smx[3] = {48, 48, 24};
    gtrace_t tr;
    game_trace(n->origin, to, smn, smx, TR_IGNORE_PLAYER | TR_IGNORE_MONSTERS, n - npcs, &tr);
    if (tr.startsolid) memcpy(n->origin, to, 12);   // squeezed: fly through rather than stick
    else {
        memcpy(n->origin, tr.endpos, 12);
        if (tr.fraction < 1) { float vn = n->bv[0] * tr.normal[0] + n->bv[1] * tr.normal[1] + n->bv[2] * tr.normal[2]; for (int k = 0; k < 3; k++) n->bv[k] -= vn * tr.normal[k]; }
    }
    memcpy(entities[n->ent].origin, n->origin, 12);
}
static void heli_next_corner(npc_t *n)
{
    int c = n->bi[0];
    const char *t = c > 0 ? entities[c].target : entities[n->ent].target;
    int nx = find_named(t);
    n->bi[0] = nx > 0 ? nx : 0;
}
static void apache_think(npc_t *n, float dt)
{
    float now = game_time();
    entity_t *e = &entities[n->ent];
    if (n->bstate == 0) {
        if (e->spawnflags & (4 | 64)) { play_named(n, "idle", false); return; }   // SF_WAITFORTRIGGER
        n->bstate = 1;
    }
    if (n->bstate == 2) {   // shot down: spin and fall, explode on impact
        n->bv[2] -= 400 * dt;
        n->yaw += 300 * dt;
        float to[3] = {n->origin[0] + n->bv[0] * dt, n->origin[1] + n->bv[1] * dt, n->origin[2] + n->bv[2] * dt};
        gtrace_t tr;
        static const float smn[3] = {-32, -32, -32}, smx[3] = {32, 32, 16};
        game_trace(n->origin, to, smn, smx, TR_IGNORE_MONSTERS, n - npcs, &tr);
        memcpy(n->origin, tr.endpos, 12);
        if (tr.fraction < 1 || tr.startsolid || now > n->bt[0]) {
            boss_explode(n, 3);
            game_radius_damage(n->origin, 300, 400, n - npcs, DMG_BLAST);
            snd_stop(n->ent, CHAN_STATIC, NULL);
            e->removed = true;
            n->solid = false;
        }
        return;
    }
    if (!n->bi[3]) { n->bi[3] = 1; heli_next_corner(n); snd_emit(n->ent, CHAN_STATIC, "apache/ap_rotor2.wav", 1, 0.3f, PITCH_NORM, NULL); }
    play_named(n, "idle", false);
    bool vis = player_alive() && dist3(n->origin, player_pos) < 3000 && can_see(n, 0, false);
    float goal[3];
    if (vis) {
        // circle the player at a distance, a little above (CApache::HuntThink m_posDesired)
        n->bt[1] += dt * 0.4f;
        float r = 520;
        goal[0] = player_pos[0] + cosf(n->bt[1]) * r; goal[1] = player_pos[1] + sinf(n->bt[1]) * r; goal[2] = player_pos[2] + 260;
        memcpy(n->last_seen, player_pos, 12);
    } else if (n->bi[0] > 0) {
        memcpy(goal, entities[n->bi[0]].origin, 12);
        if (dist3(goal, n->origin) < 160) heli_next_corner(n);
    } else memcpy(goal, n->origin, 12);
    heli_move(n, goal, vis ? 300 : 400, 250, dt);
    // face the player when fighting, else the way we fly
    float sp = sqrtf(n->bv[0] * n->bv[0] + n->bv[1] * n->bv[1]);
    float wy = vis ? yaw_to(n->origin, player_pos) : (sp > 40 ? atan2f(n->bv[1], n->bv[0]) * 180 / (float)M_PI : n->yaw);
    face_yaw(n, wy, 80, dt);
    if (!vis) return;
    float aim = fabsf(angle_diff(yaw_to(n->origin, player_pos), n->yaw));
    // chaingun bursts
    if (aim < 25 && now >= n->next_attack) {
        if (n->bi[2] <= 0) { n->bi[2] = 8 + rand() % 8; }
        if (--n->bi[2] <= 0) n->next_attack = now + 1.5f + (rand() % 100) / 100.0f;
        else n->next_attack = now + 0.1f;
        snd_emit(n->ent, CHAN_WEAPON, "turret/tu_fire1.wav", 0.8f, 0.3f, PITCH_NORM, NULL);
        fire_bullet_at(n, 0, skill_get("sk_12mm_bullet") > 0 ? skill_get("sk_12mm_bullet") : 8, 0.08f, 1);
    }
    // rockets now and then
    if (aim < 15 && now >= n->bt[2]) {
        n->bt[2] = now + 4 + rand() % 3;
        float a = (n->yaw + ((n->bi[1] ^= 1) ? 90 : -90)) * (float)M_PI / 180;
        float src[3] = {n->origin[0] + cosf(a) * 64, n->origin[1] + sinf(a) * 64, n->origin[2] - 32};
        float dir[3] = {player_pos[0] - src[0], player_pos[1] - src[1], player_pos[2] - src[2]};
        float l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (int k = 0; k < 3; k++) dir[k] /= l > 1 ? l : 1;
        proj_spawn(3 /* P_ROCKET */, src, dir, (n - npcs) + 1, -1);
        snd_emit(n->ent, CHAN_ITEM, "weapons/rocketfire1.wav", 1, 0.5f, PITCH_NORM, NULL);
    }
}
static bool heli_damage(npc_t *n, float dmg, int dmgtype)
{
    if (n->bstate == 2) return true;
    if (n->bstate == 0) n->bstate = 1;
    if (dmgtype & DMG_BLAST) dmg *= 2;               // CApache::TakeDamage
    else if (!(dmgtype & (DMG_ENERGYBEAM | DMG_SHOCK))) dmg *= 0.15f;   // small arms barely scratch it
    n->health -= dmg;
    if (n->health <= 0) {
        n->bstate = 2;
        n->bt[0] = game_time() + 5;
        n->bv[2] = fminf(n->bv[2], 0);
        ai_trigger(n, 4);   // AITRIGGER_DEATH
    }
    return true;
}

// ---- Osprey: flies its path_corner chain; at a corner with speed 0 it hovers and drops four
// grunts (CBaseMonster *MakeGrunt on ropes), then flies on and leaves at the end of the path.
static void osprey_think(npc_t *n, float dt)
{
    float now = game_time();
    entity_t *e = &entities[n->ent];
    if (n->bstate == 0) {
        if (e->spawnflags & 64) { play_named(n, "rotor", false); return; }
        n->bstate = 1;
    }
    play_named(n, "rotor", false);
    if (n->bstate == 4) {   // shot down
        n->bv[2] -= 400 * dt;
        for (int k = 0; k < 3; k++) n->origin[k] += n->bv[k] * dt;
        gtrace_t tr;
        float dn[3] = {n->origin[0], n->origin[1], n->origin[2] - 32};
        game_trace(n->origin, dn, NULL, NULL, TR_IGNORE_MONSTERS | TR_IGNORE_PLAYER, n - npcs, &tr);
        if (tr.fraction < 1 || now > n->bt[0]) { boss_explode(n, 4); snd_stop(n->ent, CHAN_STATIC, NULL); e->removed = true; n->solid = false; }
        return;
    }
    if (!n->bi[3]) { n->bi[3] = 1; heli_next_corner(n); snd_emit(n->ent, CHAN_STATIC, "apache/ap_rotor4.wav", 1, 0.3f, 90, NULL); }
    if (n->bstate == 2) {   // hovering over the drop point
        if (n->bi[2] == 0 && now >= n->bt[0] - 3) {
            n->bi[2] = 1;
            static const float ofs[4][2] = {{-40, -40}, {40, -40}, {-40, 40}, {40, 40}};
            for (int k = 0; k < 4; k++) {
                float o[3] = {n->origin[0] + ofs[k][0], n->origin[1] + ofs[k][1], n->origin[2] - 80};
                float dn[3] = {o[0], o[1], o[2] - 2048};
                gtrace_t tr;
                static const float gmn[3] = {-16, -16, 0}, gmx[3] = {16, 16, 72};
                game_trace(o, dn, gmn, gmx, TR_IGNORE_MONSTERS | TR_IGNORE_PLAYER, -1, &tr);
                if (!tr.startsolid) npc_spawn_monster("monster_human_grunt", tr.endpos, n->yaw, "", 0);
            }
        }
        if (now >= n->bt[0]) { n->bstate = 1; heli_next_corner(n); }
        for (int k = 0; k < 3; k++) n->bv[k] *= 0.9f;
        return;
    }
    if (n->bi[0] <= 0) { snd_stop(n->ent, CHAN_STATIC, NULL); e->removed = true; n->solid = false; return; }   // flew off
    entity_t *c = &entities[n->bi[0]];
    float spd = key_float(n->bi[0], "speed", 0);
    heli_move(n, c->origin, spd > 0 ? spd : 300, 200, dt);
    float sp = sqrtf(n->bv[0] * n->bv[0] + n->bv[1] * n->bv[1]);
    if (sp > 40) face_yaw(n, atan2f(n->bv[1], n->bv[0]) * 180 / (float)M_PI, 45, dt);
    if (dist3(c->origin, n->origin) < 96) {
        if (spd == 0 && n->bi[2] == 0) { n->bstate = 2; n->bt[0] = now + 6; }
        else heli_next_corner(n);
    }
}
static bool osprey_damage(npc_t *n, float dmg, int dmgtype)
{
    if (n->bstate == 4) return true;
    if (!(dmgtype & (DMG_BLAST | DMG_ENERGYBEAM))) dmg *= 0.1f;
    n->health -= dmg;
    if (n->health <= 0) { n->bstate = 4; n->bt[0] = game_time() + 5; }
    return true;
}

// a map trigger aimed at a boss: wakes it (CNihilanth/CApache/COsprey use functions)
bool npc_used(int ent)
{
    int i = npc_for_entity(ent);
    if (i < 0) return false;
    npc_t *n = &npcs[i];
    if (n->bkind != 1 && n->bkind != 3 && n->bkind != 4) return false;
    if (n->bkind == 1 && n->bstate == 3) {   // dead: the end of the game's last map (n_end_relay)
        extern bool ents_teleport_player(const char *name);
        ents_teleport_player("n_ending");
        return true;
    }
    if (n->bstate == 0) n->bstate = 1;
    return true;
}
// Xen flora: CXenPLight pulls in when the player comes close, CXenTree swats at them
static void xen_think(npc_t *n, float dt)
{
    float now = game_time();
    float d = dist2d(n->origin, player_pos);
    if (n->bkind == 5) {
        bool near = d < 80 && player_alive();
        if (near && n->bstate == 0) { play_named(n, "retract", true); n->bstate = 1; }
        else if (n->bstate == 1 && n->seq_done) { play_named(n, "hide", true); n->bstate = 2; }
        else if (!near && n->bstate == 2 && now > n->bt[0]) { play_named(n, "delpoy", true); n->bstate = 3; }
        else if (n->bstate == 3 && n->seq_done) { play_named(n, "idle1", true); n->bstate = 0; }
        if (near) n->bt[0] = now + 1.5f;
        return;
    }
    // tree: a swat when the player is in front of it, within reach
    float yaw = yaw_to(n->origin, player_pos);
    bool infront = fabsf(angle_diff(yaw, n->yaw)) < 60;
    if (n->bstate == 1) {
        if (n->attack_hit_time > 0 && now >= n->attack_hit_time) {
            n->attack_hit_time = 0;
            if (d < 120 && infront && player_alive()) player_take_damage(25, DMG_CRUSH | DMG_SLASH, n->origin, n->ent);
            snd_emit(n->ent, CHAN_WEAPON, rand() & 1 ? "zombie/claw_strike1.wav" : "zombie/claw_miss1.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        }
        if (n->seq_done) { play_named(n, "idle1", true); n->bstate = 0; }
        return;
    }
    if (d < 100 && infront && player_alive() && now > n->next_attack && fabsf(player_pos[2] - n->origin[2]) < 120) {
        play_named(n, "attack", true);
        n->bstate = 1;
        n->attack_hit_time = now + 0.4f;
        n->next_attack = now + 1.5f;
    }
}

static bool boss_think(npc_t *n, float dt)
{
#ifdef BOSSDEBUG
    { static float nd[MAX_NPCS]; int i = n - npcs; if (game_time() > nd[i]) { nd[i] = game_time() + 1;
      debugf("boss %d state %d hp %.0f pos %.0f %.0f %.0f seq %d bi %d %d %d %d\n", n->bkind, n->bstate, n->health, n->origin[0], n->origin[1], n->origin[2], n->seq, n->bi[0], n->bi[1], n->bi[2], n->bi[3]); } }
#endif
    switch (n->bkind) {
    case 1: nihilanth_think(n, dt); return true;
    case 2: if (n->ai == AI_DYING || n->ai == AI_DEAD) return false; bigmomma_think(n, dt); return true;
    case 3: apache_think(n, dt); return true;
    case 4: osprey_think(n, dt); return true;
    case 5: case 6: xen_think(n, dt); return true;
    }
    return false;
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

// CBarney's "gun" body group: 0 in the holster, 1 in his hand (2: gone)
static void barney_gun(npc_t *n, int state)
{
    studio_t *m = n->model;
    if (!m || m->numbodyparts < 2 || m->bodyparts[1].nummodels < 2) return;
    int base = m->bodyparts[1].base ? m->bodyparts[1].base : 1;
    int cur = (n->body / base) % m->bodyparts[1].nummodels;
    if (cur == 2 || cur == state) return;   // no gun at all (scripted) / already so
    n->body += (state - cur) * base;
    if (state) n->gun_time = game_time();
}

void npcs_think(float dt)
{
    float now = game_time();
    pm_get_origin(player_pos);
    player_get_eye(player_eye);
#ifdef SWARMDEBUG
    { static float shot, rep; if (shot == 0 && now > 4) { shot = now; npc_hear(player_eye, 1000, 0); debugf("SWARM: bang\n"); }
      if (now > rep) { rep = now + 1; int c = 0, a = 0, h = 0; float md = 1e9f;
        for (int k = 0; k < num_npcs; k++) { if (!npc_alive(k) || !hates(&npcs[k], 0)) continue; h++;
            if (npcs[k].ai == AI_COMBAT && npcs[k].enemy == 0) { c++; md = fminf(md, dist3(npcs[k].origin, player_pos)); }
            else if (now < npcs[k].alert_until) a++; }
        debugf("SWARM t=%.0f hostile=%d combat=%d alert=%d nearest_combat=%.0f\n", now, h, c, a, md); } }
#endif
    // the player's PVS, for "can the player see me" checks (FIND_CLIENT_IN_PVS)
    int pl = bsp_leaf_for_point(player_eye);
    if (pl != pvs_leaf) {   // decompress only when the player changes leaf (reset at map start)
        pvs_leaf = pl;
        const uint8_t *pvs = pl > 0 ? bsp_leaf_pvs(pl) : NULL;
        int bytes = (world.numleafs + 7) / 8;
        pvs_ok = pvs && bytes <= (int)sizeof(pvs_buf);
        if (pvs_ok) memcpy(pvs_buf, pvs, bytes);
    }
    scripts_retry();
#ifdef NPCDEBUG
    { static float nd; if (now > nd) { nd = now + 1; for (int i = 0; i < num_npcs; i++) if (npcs[i].script >= 0 || npcs[i].following || npcs[i].ai)
        debugf("  npc %s at %.0f %.0f %.0f ss %d ai %d path %d/%d stuck %.1f\n", entities[npcs[i].ent].targetname, npcs[i].origin[0], npcs[i].origin[1], npcs[i].origin[2], npcs[i].sstate, npcs[i].ai, npcs[i].pathi, npcs[i].npath, npcs[i].stuck_time); } }
#endif

    extern int trace_frames;
#ifdef PROFX
    static uint32_t pn[MAX_NPCS]; static int pframes; uint32_t tl = TICKS_READ(); int pi = -1;
    static uint32_t ploop; uint32_t tloop0 = tl;
    static uint32_t pq[4];
#endif
    const float frame_dt = dt;
    for (int i = 0; i < num_npcs; i++) {
        npc_t *n = &npcs[i];
        dt = frame_dt;
#ifdef PROFX
        { uint32_t t = TICKS_READ(); if (pi >= 0) pn[pi] += TICKS_DISTANCE(tl, t); tl = t; pi = i; }
#endif
        if (entities[n->ent].removed) continue;
        if (n->flags & NF_ITEM) { item_think(n, i); continue; }
        studio_t *m = n->model;
        sseq_t *s = &m->seqs[n->seq];
        // Barney puts the pistol away a while after the fight (CBarney: m_fGunDrawn without an enemy)
        if (n->gun_time > 0 && n->enemy < 0 && now - n->gun_time > 6 && npc_alive(i)) { barney_gun(n, 0); n->gun_time = 0; }
        else if (n->gun_time > 0 && n->enemy >= 0) n->gun_time = now;
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

        // idle monsters the player can't see think a few times a second (HL: no Look() outside the
        // PVS); scripts, combat, followers and anything moving keep full rate
        n->think_dt += dt;
        if (n->script < 0 && n->sstate == NS_IDLE && n->ai == AI_IDLE && !n->following && n->enemy < 0 && n->npath == 0 &&
            n->onground && !player_in_pvs(n)) {
            if (now < n->next_dormant) continue;
            n->next_dormant = now + 0.25f + (rand() % 100) / 1000.0f;
        }
        dt = n->think_dt;
        n->think_dt = 0;

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
                npc_relight(n, now);
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
        if (n->bkind && boss_think(n, dt)) continue;

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
            // bodies of monsters made during play fade after a while (frees their slots)
            if (n->spawned && now > n->dead_time + 15) { entities[n->ent].removed = true; n->solid = false; }
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
                npc_relight(n, now);
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

#ifdef PROFX
        uint32_t q0 = TICKS_READ();
#endif
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
                        else if (n->weapons && !strcmp(d->model, "hgrunt") && ok_to_speak(n)) npc_say(n, e == 0 ? "HG_ALERT" : "HG_MONST", 2, 1, ATTN_NORM);
                        if (n->flags & NF_SCI) { n->ai = AI_FLEE; n->fear_time = now; n->said &= ~32; n->next_plan = n->flee_look = 0; }
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
#ifdef PROFX
        uint32_t q1 = TICKS_READ();
#endif
        switch (n->ai) {
        case AI_IDLE: ai_idle(n, dt); break;
        case AI_COMBAT: ai_combat(n, dt); break;
        case AI_FLEE: ai_flee(n, dt); break;
        default: break;
        }
#ifdef PROFX
        uint32_t q2 = TICKS_READ();
#endif
        gravity(n, dt);
#ifdef PROFX
        uint32_t q3 = TICKS_READ();
#endif
        if (!(n->flags & NF_STATIC)) npc_relight(n, now);
#ifdef PROFX
        pq[0] += TICKS_DISTANCE(q0, q1); pq[1] += TICKS_DISTANCE(q1, q2); pq[2] += TICKS_DISTANCE(q2, q3); pq[3] += TICKS_DISTANCE(q3, TICKS_READ());
#endif
    }
#ifdef PROFX
    { uint32_t t = TICKS_READ(); if (pi >= 0) pn[pi] += TICKS_DISTANCE(tl, t); ploop += TICKS_DISTANCE(tloop0, t); }
    if (++pframes >= 30) {
        debugf("  npc prof loop %d us/frame:", (int)TICKS_TO_US(ploop / pframes)); ploop = 0;
        for (int k = 0; k < num_npcs; k++) if (pn[k] / pframes > TICKS_FROM_US(300))
            debugf(" %s[%s ai%d ss%d]=%d", entities[npcs[k].ent].classname + 8, entities[npcs[k].ent].targetname, npcs[k].ai, npcs[k].sstate, (int)TICKS_TO_US(pn[k] / pframes));
        { extern uint32_t prof_trace_ticks, prof_trace_n; debugf(" traces %d/frame %d us/frame", (int)(prof_trace_n / pframes), (int)TICKS_TO_US(prof_trace_ticks / pframes)); prof_trace_n = prof_trace_ticks = 0;
          extern int prof_nsolid(void); debugf(" solids %d entsz %d", prof_nsolid(), (int)sizeof(entity_t));
          extern uint32_t prof_tw[4]; debugf(" [bt %d world %d ents %d npcs %d]", (int)(prof_tw[3] / pframes), (int)TICKS_TO_US(prof_tw[0] / pframes), (int)TICKS_TO_US(prof_tw[1] / pframes), (int)TICKS_TO_US(prof_tw[2] / pframes)); memset(prof_tw, 0, 16); }
        debugf(" (of %d) look %d ai %d grav %d light %d\n", num_npcs, (int)TICKS_TO_US(pq[0] / pframes), (int)TICKS_TO_US(pq[1] / pframes), (int)TICKS_TO_US(pq[2] / pframes), (int)TICKS_TO_US(pq[3] / pframes));
        memset(pq, 0, sizeof(pq));
        memset(pn, 0, sizeof(pn)); pframes = 0;
    }
#endif
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
typedef struct { float o[3], yaw, health; uint8_t gone, following, bstate, pad; } nsnap_t;
int npcs_snapshot(uint8_t *b, int max)
{
    int pos = 4, n = 0;
    for (int i = 0; i < base_npcs && pos + (int)sizeof(nsnap_t) <= max; i++) {
        npc_t *c = &npcs[i];
        nsnap_t r = {.yaw = c->yaw, .health = c->health, .gone = entities[c->ent].removed || is_dead(c), .following = c->following, .bstate = (uint8_t)c->bstate};
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
        if ((c->bkind == 1 || c->bkind == 3 || c->bkind == 4) && r.bstate) c->bstate = 1;   // a boss that was awake stays awake (path restarts)
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
int npc_autoaim_target = -1;   // the monster the aim was bent onto this frame (-1 none)
bool npc_autoaim(const float *eye, float *dir)
{
    float best = 0.9925f;   // ~7 degrees
    float bestdir[3];
    bool found = false;
    npc_autoaim_target = -1;
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
        npc_autoaim_target = i;
    }
    if (found) memcpy(dir, bestdir, 12);
    return found;
}
