#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "studio.h"

#define MAX_NPCS 160
#define MAX_PATH_PTS 24

typedef struct {
    int ent;
    int def;                 // index into the monster table
    studio_t *model;
    float origin[3], yaw;
    float mins[3], maxs[3];
    int seq, idle_seq, last_frame;
    float frame, framerate;
    bool seq_done;           // m_fSequenceFinished
    int body, skin;
    uint16_t flags;
    bool solid;
    uint8_t light[3];
    // scripted sequence state
    int script, sstate;
    bool script_idle_only;
    float dest[3], destyaw, speed;
    float script_wait;       // retry time for a script waiting for this monster to be free
    // AI
    int ai;                  // AI state
    float health, maxhealth;
    float corpse_hp;        // CBaseMonster::BecomeDead: max_health / 2 (placed bodies 8), worn down by DMG_GIB_CORPSE
    int enemy;               // -1 none, 0 player, n+1 = npc n
    float next_think, next_attack, next_look, pain_time, attack_hit_time;
    bool attack_hit_done;
    int attack_kind;
    float last_seen[3];
    float vz;                // falling speed
    bool onground, moved;
    int ground_ent;
    float ground_prev[3];
    // routes
    float path[MAX_PATH_PTS][3];
    int npath, pathi;
    float goal[3];
    float stuck_time, moved_check, last_pos[3];
    int stuck_count;
    bool running;
    // talking monsters
    bool following, provoked;
    int hits_by_player;
    float use_time, talk_time, idle_speak_time, fear_time, heal_time;
    int talk_target;         // entity index looked at while talking (-1 none, 0 player)
    uint16_t said;
    int nspeak;
    int pitch;
    int voice;               // sound slot of the current sentence
    float dead_time;
    int trig_cond;           // AI trigger condition (TriggerCondition), -1 none / already fired
    float trig_check;
    float att[1][3];         // the gun's muzzle (model attachment 0) where it was last drawn
    float att_time;
    float alert_until, investigate, heard[3];   // a noise was heard (walk over to it until alert_until)
    float ground_check;      // next look at the ground when standing still on the world
    float gun_time;          // Barney: when the pistol was last needed (0: holstered)
    float ctl0;              // bone controller 0 (barnacle: tongue length)
    float alt;               // barnacle: how far down the tongue reaches
    float tongue_len;        // barnacle: distance to the floor (traced once)
    float next_dormant, think_dt;   // dormant (idle, out of the player's PVS): AI a few times a second
    float light_pos[3], light_time;
    float leaf_org[3]; int leaf_cache;
    int maker;               // monstermaker entity that made it (0 none)
    bool spawned;            // made during play (its slots can be reused once the body is gone)
    int16_t bone_cache;      // studio.c static pose slot (-1 none)
    float bone_cache_org[4]; // where/how it was when cached (origin, yaw)
    int bkind, bstate, bi[4];    // bosses (nihilanth, big momma, apache, osprey): kind, state, ints
    float bt[4], bv[3];          //   timers, flying velocity
    int weapons;                 // human grunt "weapons" bits (1 mp5, 2 hand grenades, 4 launcher, 8 shotgun)
    float next_grenade, cover_until;
    bool vis_cache; int vis_target; float vis_time;   // combat line-of-sight cache   // BSP leaf at leaf_org (PVS checks)
} npc_t;

typedef struct { float origin[3], mins[3], maxs[3]; } npc_box_t;

extern npc_t npcs[MAX_NPCS];
extern int num_npcs;

void npcs_spawn_all(void);
void npc_scripts_init(void);
void npc_script_use(int script_entity);
void npcs_think(float dt);
int  npcs_get_boxes(npc_box_t *out, int max, const float *near, float range);
void npcs_save_transition(const char *landmark, const float *landmark_origin);
void npcs_restore_transition(const float *landmark_origin);

// combat
void npc_take_damage(int npc, float dmg, int dmgtype, const float *dir, int attacker); // attacker: 0 player, n+1 npc
void npc_hear(const float *origin, float radius, int source);                         // gunfire, explosions
bool npc_is_item(int npc);
bool npc_alive(int npc);
float npc_eye_height(int npc);
