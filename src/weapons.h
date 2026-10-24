#pragma once
#include <stdbool.h>
#include <stdint.h>

// dlls/weapons.h ids
#define WEAPON_CROWBAR      1
#define WEAPON_GLOCK        2
#define WEAPON_PYTHON       3
#define WEAPON_MP5          4
#define WEAPON_CROSSBOW     6
#define WEAPON_SHOTGUN      7
#define WEAPON_RPG          8
#define WEAPON_GAUSS        9
#define WEAPON_EGON         10
#define WEAPON_HORNETGUN    11
#define WEAPON_HANDGRENADE  12
#define WEAPON_TRIPMINE     13
#define WEAPON_SATCHEL      14
#define WEAPON_SNARK        15
#define WEAPON_SUIT         31

bool weapons_give(const char *cls);
bool weapons_give_ammo(const char *cls);
void weapons_reset(void);
void weapons_map_start(void);
void weapons_clear_world(void);
int  weapons_current(void);
void weapons_think(float dt, const float *view_angles, bool fire, bool fire2, bool reload, int select_dir, bool can_fire);
void weapons_draw3d(const float *eye, const float *angles, const uint8_t *light);
void weapons_draw_effects(void);
void weapons_draw_hud(float dt, bool show_crosshair);
void weapons_aim(float *f);
int  hud_number(int x, int y, int value, int digits, bool draw_zero);
void hud_icon2(int x, int y, int sx, int sy, int w, int h);
void weapons_hud_init(void);
