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
    uint8_t bind[9];       // button for each action (BIND_*), PAD_* values
} settings_t;

// remappable actions and the pad buttons they can go on
enum { BIND_FIRE, BIND_ALTFIRE, BIND_JUMP, BIND_DUCK, BIND_USE, BIND_RELOAD, BIND_FLASHLIGHT, BIND_PREVWEAP, BIND_NEXTWEAP, NUM_BINDS };
enum { PAD_A, PAD_B, PAD_Z, PAD_L, PAD_R, PAD_DU, PAD_DD, PAD_DL, PAD_DR, PAD_CU, PAD_CD, PAD_CL, PAD_CR,
       PAD_SU, PAD_SD, PAD_SL, PAD_SR,   // S*: the stick pushed that way (schemes that leave it free)
       PAD_X, PAD_Y, NUM_PADS };          // a GameCube controller's X and Y
#define NUM_SCHEMES 5
void settings_fit_binds(void);
void pad_stick_update(const joypad_inputs_t *in);
extern const char *bind_names[NUM_BINDS];
extern const char *pad_names[NUM_PADS];
void settings_default_binds(void);
bool pad_held(const joypad_inputs_t *in, int pad);
bool pad_pressed(const joypad_buttons_t *b, int pad);
void settings_save(void);    // to the cartridge's SRAM (after the save slots)
void settings_load(void);
extern settings_t settings;

typedef enum { MENU_ACT_NONE, MENU_ACT_RESUME, MENU_ACT_START_MAP, MENU_ACT_LEAVE } menu_action_t;

void menu_init(void);
void menu_open(bool in_game);
void menu_close(void);
bool menu_is_open(void);
menu_action_t menu_update(float dt, joypad_inputs_t in, joypad_buttons_t pressed, char *map_out, int map_out_len);
void menu_draw(void);                        // 2D layer (the caller draws the background)
void menu_draw_loading(float progress);      // the "Loading..." dialog on a black screen

bool menu_debug_warped(void);   // the last START_MAP came from the Debug map loader
