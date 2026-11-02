// HL64 - Head-To-Head (h2h.c)
#pragma once
#include <stdbool.h>

#define H2H_NUM_CHARS 6
#define H2H_NUM_MAPS 11
extern const char *const h2h_char_names[H2H_NUM_CHARS];
extern const char *const h2h_maps[H2H_NUM_MAPS];
extern const char *const h2h_map_titles[H2H_NUM_MAPS];
extern const char *const h2h_map_credits[H2H_NUM_MAPS];   // community maps: who made it (NULL: Gearbox's)

void h2h_begin(int char1, int char2, int frags, int minutes);   // after the map is loaded
void h2h_end(void);
void h2h_switch(int player);
void h2h_turn_begin(int player);
bool h2h_turn_dead(int player, bool any_button);
void h2h_turn_end(int player, const float *eye, const float *ang, float dt, bool firing);
void h2h_view(int player, float *eye, float *ang);
bool h2h_think(float dt, bool any_button);
void h2h_draw_hud(int player);
void h2h_player_died(bool gibbed);
int  h2h_frags(int player);
bool h2h_intermission(void);
