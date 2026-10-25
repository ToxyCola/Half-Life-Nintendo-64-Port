// HL64 - menus modelled on Half-Life's GameUI (the Steam version's main menu and dialogs).
//
// Main menu (GameMenu.res): logo, then "New game / Load game / Options" (title screen) or
// "Resume game / Load game / Options / Leave game" (in game), in the MenuLarge colours from
// TrackerScheme.res with the blurred highlight on the selected item, and the hint line at
// the bottom. Dialogs use the VGUI scheme colours (ControlBG, BorderBright/Dark, SelectionBG).
// Text is Half-Life's own font (gfx.wad CREDITSFONT) - see font.c.
//
// Pad: D-pad / stick to move, A to choose, B to go back, START to resume, Z/R to change tabs.
#ifndef HUD_SAFE_X
#define HUD_SAFE_X 12
#define HUD_SAFE_Y 9
#endif
#include <libdragon.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "font.h"
#include "menu.h"

settings_t settings = {.control_scheme = 0, .invert_y = false, .look_sens = 5, .volume = 8,
                       .show_stats = false, .crosshair = false, .cheats = false, .skill = 2};

#define SW 320
#define SH 240

// TrackerScheme.res
#define C_MENU        RGBA32(238, 174, 0, 255)
#define C_MENU_ARMED  RGBA32(255, 186, 0, 255)
#define C_MENU_BLUR   RGBA32(255, 178, 67, 255)
#define C_MENU_HINT   RGBA32(104, 104, 104, 255)
#define C_CONTROL_BG  RGBA32(76, 88, 68, 255)
#define C_WINDOW_BG   RGBA32(62, 70, 55, 255)
#define C_BORDER_BRT  RGBA32(136, 145, 128, 255)
#define C_BORDER_DARK RGBA32(40, 46, 34, 255)
#define C_TEXT        RGBA32(216, 222, 211, 255)
#define C_TEXT_DIM    RGBA32(160, 170, 149, 255)
#define C_TEXT_BRIGHT RGBA32(255, 255, 255, 255)
#define C_SEL_BG      RGBA32(149, 136, 49, 255)
#define C_CTRL_BRIGHT RGBA32(196, 181, 80, 255)

typedef enum { SCR_MAIN, SCR_NEWGAME, SCR_LOAD, SCR_OPTIONS, SCR_LEAVE } screen_t;

static bool open, in_game;
static screen_t screen;
static int main_sel, new_sel = 2, load_sel, load_scroll, opt_tab, opt_sel, leave_sel;
static float anim_t, repeat_t;
static int repeat_dir;
static image_t logo;

static char chapters[24][40];
static char chapter_maps[24][16];
static int num_chapters;

// ------------------------------------------------------------------ drawing helpers
static void fill(int x, int y, int w, int h, color_t c)
{
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_set_prim_color(c);
    rdpq_fill_rectangle(x, y, x + w, y + h);
}

static void bevel(int x, int y, int w, int h, color_t lit, color_t dark)
{
    fill(x, y, w, 1, lit);
    fill(x, y, 1, h, lit);
    fill(x, y + h - 1, w, 1, dark);
    fill(x + w - 1, y, 1, h, dark);
}

static void text(int f, int x, int y, color_t c, const char *s)
{
    font_begin(BLEND_ALPHA);
    font_color(c);
    font_draw(f, x, y, s);
}

static void text_c(int f, int cx, int y, color_t c, const char *s)
{
    font_begin(BLEND_ALPHA);
    font_color(c);
    font_draw_centered(f, cx, y, s);
}

// VGUI Frame: background, raised border, title in the top-left
static void frame(int x, int y, int w, int h, const char *title)
{
    fill(x, y, w, h, C_CONTROL_BG);
    bevel(x, y, w, h, C_BORDER_BRT, C_BORDER_DARK);
    text(FONT_SMALL, x + 8, y + 5, C_TEXT_BRIGHT, title);
}

// VGUI Button: armed buttons get the selection colours and the black "default" border
static void button(int x, int y, int w, const char *label, bool armed)
{
    int h = 15;
    fill(x, y, w, h, armed ? C_SEL_BG : C_CONTROL_BG);
    bevel(x, y, w, h, C_BORDER_BRT, C_BORDER_DARK);
    if (armed) bevel(x - 1, y - 1, w + 2, h + 2, RGBA32(0, 0, 0, 255), RGBA32(0, 0, 0, 255));
    text_c(FONT_SMALL, x + w / 2, y + 2, armed ? C_TEXT_BRIGHT : C_TEXT, label);
}

// radio button / list row
static void radio(int x, int y, const char *label, bool on, bool armed)
{
    fill(x, y + 2, 8, 8, C_WINDOW_BG);
    bevel(x, y + 2, 8, 8, C_BORDER_DARK, C_BORDER_BRT);
    if (on) fill(x + 2, y + 4, 4, 4, C_CTRL_BRIGHT);
    if (armed) fill(x + 12, y, font_width(FONT_SMALL, label) + 6, 12, C_SEL_BG);
    text(FONT_SMALL, x + 15, y + 1, armed ? C_TEXT_BRIGHT : C_TEXT, label);
}

// word-wrapped dim help text
static void help_text(int x, int y, int w, const char *s)
{
    char line[96];
    int lh = font_height(FONT_SMALL);
    while (*s) {
        int n = 0, last_space = -1;
        while (s[n] && s[n] != '\n' && n < 95) {
            line[n] = s[n];
            line[n + 1] = 0;
            if (s[n] == ' ') last_space = n;
            if (font_width(FONT_SMALL, line) > w && last_space > 0) { n = last_space; break; }
            n++;
        }
        line[n] = 0;
        text(FONT_SMALL, x, y, C_TEXT_DIM, line);
        y += lh;
        s += n;
        while (*s == ' ' || *s == '\n') s++;
    }
}

// ------------------------------------------------------------------ setup
void menu_init(void)
{
    image_load(&logo, "rom:/ui/logo.sprite");
    FILE *f = fopen("rom:/maps/chapters.txt", "r");
    if (f) {
        char l[80];
        while (fgets(l, sizeof(l), f) && num_chapters < 24) {
            char *sp = strchr(l, ' ');
            if (!sp) continue;
            *sp = 0;
            char *t = sp + 1;
            t[strcspn(t, "\r\n")] = 0;
            if (!strcmp(l, "t0a0")) continue;   // the Training Room is in New Game
            strncpy(chapter_maps[num_chapters], l, 15);
            strncpy(chapters[num_chapters], t, 39);
            num_chapters++;
        }
        fclose(f);
    }
}

void menu_open(bool ig)
{
    open = true;
    in_game = ig;
    screen = SCR_MAIN;
    main_sel = 0;
    anim_t = 0;
}

void menu_close(void) { open = false; }
bool menu_is_open(void) { return open; }

// ------------------------------------------------------------------ main menu items
bool game_save_exists(void);
void game_save_manual(void);
int game_saves_list(int *slots, char (*labels)[48]);
static int num_saves, save_slots[6];
static char save_labels[6][48];
typedef enum { MI_CONTINUE, MI_RESUME, MI_SAVE, MI_NEWGAME, MI_LOAD, MI_OPTIONS, MI_LEAVE } mitem_t;
static const char *mi_label[] = {"Continue", "Resume game", "Save game", "New game", "Load game", "Options", "Leave game"};
static const char *mi_hint[] = {"Continue from your most recent save.", "", "Save your progress here.", "Start a new single player game.", "Start from the beginning of a chapter.",
                                "Change game settings, configure controls.", ""};

static int main_items(mitem_t *out)
{
    int n = 0;
    if (in_game) { out[n++] = MI_RESUME; out[n++] = MI_SAVE; }
    else { if (game_save_exists()) out[n++] = MI_CONTINUE; out[n++] = MI_NEWGAME; }      // GameMenu.res: New game is hidden while playing
    out[n++] = MI_LOAD;
    out[n++] = MI_OPTIONS;
    if (in_game) out[n++] = MI_LEAVE;
    return n;
}

// ------------------------------------------------------------------ options
typedef struct { const char *label; int *ival; bool *bval; int min, max; const char **names; } opt_t;
static const char *scheme_names[] = {"Stick moves, C looks", "Stick looks, C moves", "Stick moves + turns"};
static const char *onoff[] = {"Off", "On"};
static const char *tab_names[] = {"Controls", "Audio", "Video", "Debug"};
extern bool dbg_god, dbg_noclip, dbg_give_all;
int game_num_maps(void);
const char *game_map_name(int i);
static bool dbg_warp;
static bool warped;
bool menu_debug_warped(void) { bool w = warped; warped = false; return w; }
static int dbg_map;
static const char *dbg_map_names[160];
static const char *dbg_action[] = {"Press A", "..."};

static int options_for_tab(int tab, opt_t *o)
{
    int n = 0;
    if (tab == 0) {
        o[n++] = (opt_t){"Control scheme", &settings.control_scheme, NULL, 0, 2, scheme_names};
        o[n++] = (opt_t){"Look sensitivity", &settings.look_sens, NULL, 1, 10, NULL};
        o[n++] = (opt_t){"Reverse up-down axis", NULL, &settings.invert_y, 0, 1, onoff};
        o[n++] = (opt_t){"Cheats", NULL, &settings.cheats, 0, 1, onoff};
    } else if (tab == 1) {
        o[n++] = (opt_t){"Game volume", &settings.volume, NULL, 0, 10, NULL};
    } else if (tab == 2) {
        o[n++] = (opt_t){"Show frame rate", NULL, &settings.show_stats, 0, 1, onoff};
    } else {
        int nm = game_num_maps();
        for (int i = 0; i < nm && i < 160; i++) dbg_map_names[i] = game_map_name(i);
        o[n++] = (opt_t){"God mode", NULL, &dbg_god, 0, 1, onoff};
        o[n++] = (opt_t){"Noclip (A up, L down)", NULL, &dbg_noclip, 0, 1, onoff};
        o[n++] = (opt_t){"Give everything (impulse 101)", NULL, &dbg_give_all, 0, 1, dbg_action};
        if (nm > 0) {
            o[n++] = (opt_t){"Map", &dbg_map, NULL, 0, nm - 1, dbg_map_names};
            o[n++] = (opt_t){"Go to map", NULL, &dbg_warp, 0, 1, dbg_action};
        }
    }
    return n;
}

static void option_change(opt_t *o, int d)
{
    if (o->bval) { *o->bval = !*o->bval; return; }
    int v = *o->ival + d;
    if (o->names) { int span = o->max - o->min + 1; v = o->min + ((v - o->min) % span + span) % span; }
    if (v < o->min) v = o->min;
    if (v > o->max) v = o->max;
    *o->ival = v;
}

// ------------------------------------------------------------------ input
static int nav_dir(float dt, joypad_inputs_t in, joypad_buttons_t pressed, bool horizontal)
{
    int d = 0;
    if (!horizontal) {
        if (pressed.d_up || pressed.c_up) return -1;
        if (pressed.d_down || pressed.c_down) return 1;
        int sy = in.stick_y;
        d = sy > 40 ? -1 : (sy < -40 ? 1 : 0);
    } else {
        if (pressed.d_left || pressed.c_left) return -1;
        if (pressed.d_right || pressed.c_right) return 1;
        int sx = in.stick_x;
        d = sx > 40 ? 1 : (sx < -40 ? -1 : 0);
    }
    // analog stick: move once, then auto-repeat
    if (d == 0) { if (repeat_dir && ((repeat_dir & 4) != 0) == horizontal) repeat_dir = 0; return 0; }
    int key = d + 2 + (horizontal ? 4 : 0);
    if (repeat_dir != key) { repeat_dir = key; repeat_t = 0.35f; return d; }
    repeat_t -= dt;
    if (repeat_t <= 0) { repeat_t = 0.1f; return d; }
    return 0;
}

menu_action_t menu_update(float dt, joypad_inputs_t in, joypad_buttons_t pressed, char *map_out, int map_out_len)
{
    anim_t += dt;
    if (!open) return MENU_ACT_NONE;
    mitem_t items[6];
    int n = main_items(items);
    switch (screen) {
    case SCR_MAIN: {
        int d = nav_dir(dt, in, pressed, false);
        main_sel = (main_sel + d + n) % n;
        if (pressed.start && in_game) { open = false; return MENU_ACT_RESUME; }
        if (pressed.b && in_game) { open = false; return MENU_ACT_RESUME; }
        if (pressed.a || pressed.start) {
            switch (items[main_sel]) {
            case MI_RESUME: open = false; return MENU_ACT_RESUME;
            case MI_CONTINUE: open = false; strncpy(map_out, "*save", map_out_len); return MENU_ACT_START_MAP;
            case MI_NEWGAME: screen = SCR_NEWGAME; break;
            case MI_LOAD: screen = SCR_LOAD; num_saves = game_saves_list(save_slots, save_labels); load_sel = load_scroll = 0; break;
            case MI_SAVE: game_save_manual(); open = false; return MENU_ACT_RESUME;
            case MI_OPTIONS: screen = SCR_OPTIONS; opt_sel = 0; break;
            case MI_LEAVE: screen = SCR_LEAVE; leave_sel = 1; break;
            }
        }
        break;
    }
    case SCR_NEWGAME: {
        // NewGameDialog.res: Training Room / Easy / Medium / Hard, then Play
        int d = nav_dir(dt, in, pressed, false);
        new_sel = (new_sel + d + 4) % 4;
        if (pressed.b) screen = SCR_MAIN;
        if (pressed.a || pressed.start) {
            open = false;
            if (new_sel == 0) strncpy(map_out, "t0a0", map_out_len);
            else { settings.skill = new_sel; strncpy(map_out, "c0a0", map_out_len); }
            return MENU_ACT_START_MAP;
        }
        break;
    }
    case SCR_LOAD: {
        int total = num_saves + num_chapters;
        if (!total) { if (pressed.b || pressed.a) screen = SCR_MAIN; break; }
        int d = nav_dir(dt, in, pressed, false);
        load_sel = (load_sel + d + total) % total;
        if (load_sel < load_scroll) load_scroll = load_sel;
        if (load_sel >= load_scroll + 8) load_scroll = load_sel - 7;
        if (pressed.b) screen = SCR_MAIN;
        if (pressed.a || pressed.start) {
            open = false;
            if (load_sel < num_saves) snprintf(map_out, map_out_len, "*save%d", save_slots[load_sel]);
            else strncpy(map_out, chapter_maps[load_sel - num_saves], map_out_len);
            return MENU_ACT_START_MAP;
        }
        break;
    }
    case SCR_OPTIONS: {
        opt_t o[8];
        int no = options_for_tab(opt_tab, o);
        if (pressed.r || pressed.z) { { int nt = settings.cheats ? 4 : 3; opt_tab = (opt_tab + (pressed.r ? 1 : nt - 1)) % nt; } opt_sel = 0; break; }
        int d = nav_dir(dt, in, pressed, false);
        opt_sel = (opt_sel + d + no) % no;
        int h = nav_dir(dt, in, pressed, true);
        if (h) option_change(&o[opt_sel], h);
        if (pressed.a) option_change(&o[opt_sel], 1);
        if (dbg_warp) {   // Debug: load the chosen map at its start point
            dbg_warp = false;
            warped = true;
            open = false;
            strncpy(map_out, game_map_name(dbg_map), map_out_len);
            return MENU_ACT_START_MAP;
        }
        if (dbg_give_all) { open = false; return MENU_ACT_RESUME; }
        if (pressed.b || pressed.start) screen = SCR_MAIN;
        break;
    }
    case SCR_LEAVE: {
        int h = nav_dir(dt, in, pressed, true);
        if (h) leave_sel ^= 1;
        if (pressed.b) screen = SCR_MAIN;
        if (pressed.a) {
            if (leave_sel == 0) { in_game = false; screen = SCR_MAIN; main_sel = 0; return MENU_ACT_LEAVE; }
            screen = SCR_MAIN;
        }
        break;
    }
    }
    return MENU_ACT_NONE;
}

// ------------------------------------------------------------------ drawing
static void draw_main(void)
{
    // logo (resource/logo.tga) across the top, as on the PC title screen
    if (logo.spr) {
        font_begin(BLEND_ALPHA);
        font_color(RGBA32(255, 255, 255, 255));
        image_draw(&logo, (SW - logo.surf.width) / 2, 44, 0, 0, logo.surf.width, logo.surf.height);
    }
    mitem_t items[6];
    int n = main_items(items);
    int x = 36, y = 112, step = 19;
    for (int i = 0; i < n; i++) {
        bool armed = i == main_sel && screen == SCR_MAIN;
        // menu items appear one after another (MenuItemVisibilityRate)
        if (anim_t < 0.06f * i) continue;
        if (armed) {
            float pulse = 0.75f + 0.25f * sinf(anim_t * 4.0f);
            font_begin(BLEND_ADD);
            font_color(RGBA32(255 * pulse * 0.55f, 178 * pulse * 0.55f, 67 * pulse * 0.55f, 255));
            font_draw(FONT_GLOW, x, y + i * step, mi_label[items[i]]);
        }
        text(FONT_MEDIUM, x, y + i * step, armed ? C_MENU_ARMED : C_MENU, mi_label[items[i]]);
    }
    if (screen == SCR_MAIN) text(FONT_SMALL, x, SH - 30, C_MENU_HINT, mi_hint[items[main_sel]]);
}

static void draw_newgame(void)
{
    int x = 70, y = 52, w = 180, h = 140;
    frame(x, y, w, h, "New Game");
    text(FONT_SMALL, x + 14, y + 22, C_TEXT, "Select Difficulty");
    static const char *opts[] = {"Training Room", "Easy", "Medium", "Hard"};
    for (int i = 0; i < 4; i++) radio(x + 18, y + 38 + i * 14, opts[i], i == new_sel, i == new_sel);
    help_text(x + 14, y + 96, w - 28, "Select a level of difficulty or the Training Room.");
    button(x + w - 128, y + h - 22, 56, "Play (A)", true);
    button(x + w - 66, y + h - 22, 56, "Cancel (B)", false);
}

static void draw_load(void)
{
    int x = 40, y = 30, w = 240, h = 184;
    frame(x, y, w, h, "Load Game");
    fill(x + 10, y + 20, w - 20, 8 * 15 + 4, C_WINDOW_BG);
    bevel(x + 10, y + 20, w - 20, 8 * 15 + 4, C_BORDER_DARK, C_BORDER_BRT);
    int total = num_saves + num_chapters;
    for (int i = 0; i < 8 && load_scroll + i < total; i++) {
        int k = load_scroll + i;
        int ry = y + 22 + i * 15;
        if (k == load_sel) fill(x + 12, ry, w - 24, 15, C_SEL_BG);
        char num[8];
        if (k < num_saves) strcpy(num, "*");
        else snprintf(num, sizeof(num), "%d", k - num_saves + 1);
        text(FONT_SMALL, x + 16, ry + 2, k == load_sel ? C_TEXT_BRIGHT : C_TEXT_DIM, num);
        text(FONT_SMALL, x + 34, ry + 2, k == load_sel ? C_TEXT_BRIGHT : C_TEXT, k < num_saves ? save_labels[k] : chapters[k - num_saves]);
    }
    if (total > 8) {   // scroll bar
        int track = 8 * 15;
        int th = track * 8 / total, ty = track * load_scroll / total;
        fill(x + w - 16, y + 22 + ty, 4, th, C_BORDER_BRT);
    }
    help_text(x + 12, y + h - 38, w - 24, "Saved games (*) first, newest at the top, then chapters.");
    button(x + w - 128, y + h - 22, 56, "Load (A)", true);
    button(x + w - 66, y + h - 22, 56, "Cancel (B)", false);
}

static void draw_options(void)
{
    int x = 24, y = 27, w = 272, h = 186;
    frame(x, y, w, h, "Options");
    // property sheet tabs
    int tx = x + 10;
    if (!settings.cheats && opt_tab == 3) opt_tab = 0;   // Debug tab only while cheats are on
    for (int t = 0; t < (settings.cheats ? 4 : 3); t++) {
        int tw = font_width(FONT_SMALL, tab_names[t]) + 14;
        fill(tx, y + 20, tw, 14, t == opt_tab ? C_CONTROL_BG : C_WINDOW_BG);
        bevel(tx, y + 20, tw, 14, C_BORDER_BRT, C_BORDER_DARK);
        text(FONT_SMALL, tx + 7, y + 22, t == opt_tab ? C_TEXT_BRIGHT : C_TEXT_DIM, tab_names[t]);
        tx += tw + 2;
    }
    text(FONT_SMALL, x + w - 72, y + 22, C_TEXT_DIM, "Z / R: tab");
    fill(x + 10, y + 34, w - 20, h - 64, C_CONTROL_BG);
    bevel(x + 10, y + 34, w - 20, h - 64, C_BORDER_BRT, C_BORDER_DARK);
    opt_t o[8];
    int no = options_for_tab(opt_tab, o);
    for (int i = 0; i < no; i++) {
        int ry = y + 40 + i * 16;
        bool armed = i == opt_sel;
        if (armed) fill(x + 14, ry - 2, w - 28, 15, C_SEL_BG);
        text(FONT_SMALL, x + 20, ry, armed ? C_TEXT_BRIGHT : C_TEXT, o[i].label);
        char v[40];
        if (o[i].bval) snprintf(v, sizeof(v), "%s", onoff[*o[i].bval ? 1 : 0]);
        else if (o[i].names) snprintf(v, sizeof(v), "%s", o[i].names[*o[i].ival]);
        else {
            // slider
            int sx = x + w - 100, sw = 70;
            fill(sx, ry + 5, sw, 3, RGBA32(31, 31, 31, 255));
            int k = (*o[i].ival - o[i].min) * sw / (o[i].max - o[i].min);
            fill(sx + k - 2, ry + 1, 5, 11, C_CTRL_BRIGHT);
            snprintf(v, sizeof(v), "%d", *o[i].ival);
            text(FONT_SMALL, sx + sw + 6, ry, C_TEXT, v);
            continue;
        }
        font_begin(BLEND_ALPHA);
        font_color(armed ? C_TEXT_BRIGHT : C_CTRL_BRIGHT);
        font_draw_right(FONT_SMALL, x + w - 22, ry, v);
    }
    if (opt_tab == 0) {
        // the controls, two columns filling the space under the options
        static const char *help[3][8] = {
            {"Stick: move", "C buttons: look", "Z: fire", "R: alt fire", "A: jump (hold: tuck)", "L: duck", "B: use", "D-pad: guns, reload, light"},
            {"Stick: look", "C buttons: move", "Z: fire", "R: alt fire", "A: jump (hold: tuck)", "L: duck", "B: use", "D-pad: guns, reload, light"},
            {"Stick: move / turn", "C buttons: look / strafe", "Z: fire", "R: alt fire", "A: jump (hold: tuck)", "L: duck", "B: use", "D-pad: guns, reload, light"},
        };
        int lh = font_height(FONT_SMALL), top = y + 40 + no * 16 + 2;
        for (int k = 0; k < 8; k++)
            text(FONT_SMALL, x + 20 + (k & 1) * 112, top + (k >> 1) * lh, C_TEXT_DIM, help[settings.control_scheme][k]);
    }
    button(x + w - 70, y + h - 22, 60, "Done (B)", true);
}

static void draw_leave(void)
{
    int x = 70, y = 80, w = 180, h = 70;
    frame(x, y, w, h, "Leave game");
    text_c(FONT_SMALL, x + w / 2, y + 24, C_TEXT, "Return to the main menu?");
    button(x + w / 2 - 62, y + h - 22, 56, "OK", leave_sel == 0);
    button(x + w / 2 + 6, y + h - 22, 56, "Cancel", leave_sel == 1);
}

// the title backdrop is 150 KB: give it back before a map loads (maps need big contiguous blocks)
static sprite_t *title_bg;
static bool title_tried;
void menu_release_title(void)
{
    if (title_bg) { rspq_wait(); sprite_free(title_bg); title_bg = NULL; }
    title_tried = false;
}
void menu_draw(void)
{
    if (!open) return;
    if (in_game) fill(0, 0, SW, SH, RGBA32(0, 0, 0, 120));   // the game shows through, dimmed
    else {
        // title screen backdrop
        if (!title_tried) { title_tried = true; title_bg = sprite_load("rom:/ui/title.sprite"); }
        sprite_t *bg = title_bg;
        if (bg) {
            rdpq_set_mode_copy(false);
            rdpq_sprite_blit(bg, 0, 0, NULL);
            rdpq_set_mode_standard();
        }
    }
    draw_main();
    switch (screen) {
    case SCR_NEWGAME: draw_newgame(); break;
    case SCR_LOAD: draw_load(); break;
    case SCR_OPTIONS: draw_options(); break;
    case SCR_LEAVE: draw_leave(); break;
    default: break;
    }
}

// LoadingDialog: "Loading..." with a progress bar
void menu_draw_loading(float progress)
{
    extern bool loading_overlay_active;
    if (loading_overlay_active) return;   // level change: the last view with LOADING stays up
    surface_t *fb = display_get();
    rdpq_attach_clear(fb, NULL);
    int w = 200, h = 54, x = (SW - w) / 2, y = (SH - h) / 2;
    frame(x, y, w, h, "Loading...");
    fill(x + 10, y + 26, w - 20, 12, C_WINDOW_BG);
    bevel(x + 10, y + 26, w - 20, 12, C_BORDER_DARK, C_BORDER_BRT);
    int pw = (int)((w - 24) * progress);
    // VGUI progress bars are drawn as separate blocks
    for (int bx = 0; bx + 6 <= pw; bx += 8) fill(x + 12 + bx, y + 28, 6, 8, C_SEL_BG);
    rdpq_detach_show();
}
