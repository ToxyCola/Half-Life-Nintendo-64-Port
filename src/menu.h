// HL64 - title screen and pause menu modelled on the PC version's GameUI
// (resource/GameMenu.res, NewGameDialog, LoadGameDialog, Options), driven by the N64 pad.
#pragma once
#include <libdragon.h>
#include <stdbool.h>

typedef struct {
    int  control_scheme;   // 0 = stick moves / C looks, 1 = stick looks / C moves, 2 = stick moves + turns
    bool invert_y;
    int  look_sens;        // 1..10
    int  volume;           // 0..10
    bool show_stats;
    bool crosshair;
    bool cheats;           // L = noclip
    int  skill;            // 1..3
} settings_t;
extern settings_t settings;

typedef enum { MENU_ACT_NONE, MENU_ACT_RESUME, MENU_ACT_START_MAP, MENU_ACT_LEAVE } menu_action_t;

void menu_init(void);
void menu_open(bool in_game);
void menu_close(void);
bool menu_is_open(void);
menu_action_t menu_update(float dt, joypad_inputs_t in, joypad_buttons_t pressed, char *map_out, int map_out_len);
void menu_draw(void);                        // 2D layer (the caller draws the background)
void menu_draw_loading(float progress);      // the "Loading..." dialog on a black screen
