// HL64 - HUD: titles.txt text messages (cl_dll/message.cpp), screen fades (env_fade),
// the game title, and the train speed indicator
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    float x, y;
    int effect;
    uint8_t r1, g1, b1, r2, g2, b2;
    float fadein, fadeout, holdtime, fxtime;
    const char *name;
    const char *text;
} textmsg_t;

void hud_init(void);
void hud_reset(void);                        // new map / back to the menu
const char *hud_title_text(const char *name); // titles.txt lookup (NULL if missing)
void hud_show_message(const char *name);      // env_message: titles.txt name, or literal text
void hud_show_text(const textmsg_t *m);       // game_text (message is copied)
void hud_show_game_title(void);
void hud_fade(const uint8_t rgba[4], float duration, float hold, int flags);
void hud_set_dark(void);                      // worldspawn "startdark"
void hud_set_train(int speedlevel);           // -1 = not driving, 0..4 = back, stop, 1/3, 2/3, full
bool hud_end_of_game(void);                   // END3 credits finished: leave the game
void hud_draw_messages(void);                 // HUD text + game title
void hud_draw_fade(void);
void hud_draw_train(void);
void hud_update(float dt);

// screen fade flags (FFADE_* from the engine)
#define FFADE_OUT      0x1
#define FFADE_MODULATE 0x2
#define FFADE_STAYOUT  0x4
