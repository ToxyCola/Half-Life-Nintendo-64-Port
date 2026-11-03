// HL64 - Half-Life's own fonts (gfx.wad CREDITSFONT) drawn with the RDP
#pragma once
#include <libdragon.h>

enum { FONT_SMALL, FONT_MEDIUM, FONT_LARGE, FONT_GLOW, FONT_COUNT };
enum { BLEND_ALPHA, BLEND_ADD };

void font_init(void);
int  font_height(int f);
bool font_ink_rows(int f, int ch, int *top, int *bottom);
int  font_width(int f, const char *s);            // up to the first newline
void font_begin(int blend);                        // set the RDP mode for text
void font_color(color_t c);
int  font_draw(int f, int x, int y, const char *s); // returns the x after the text (single line)
int  font_draw_scaled(int f, int x, int y, const char *s, float scale);
int  font_draw_char(int f, int x, int y, int ch);  // returns the advance (pixels, rounded)
int  font_draw_centered(int f, int cx, int y, const char *s);
int  font_draw_right(int f, int rx, int y, const char *s);

// button icons inside text: one byte, FONT_ICON(i) (0x0E..0x1F), for icon i = a PAD_* value
// (menu.h: A, B, Z, L, R, D-pad, C buttons, stick directions) or ICON_START
#define ICON_START 17
#define FONT_NUM_ICONS 18
#define FONT_ICON(i) ((char)(0x0E + (i)))
#define FONT_IS_ICON(c) ((unsigned char)(c) >= 0x0E && (unsigned char)(c) < 0x0E + FONT_NUM_ICONS)

// I4 images tinted by the current font colour
typedef struct { sprite_t *spr; surface_t surf; } image_t;
bool image_load(image_t *im, const char *path);
void image_draw(image_t *im, int x, int y, int sx, int sy, int w, int h);
