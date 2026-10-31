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
void snd_play(const char *sample, float volume, int channel);
void snd_set_volume(float v);

settings_t settings = {.control_scheme = 0, .invert_y = false, .look_sens = 5, .volume = 8,
                       .show_stats = false, .crosshair = false, .cheats = false, .skill = 2,
                       .bind = {PAD_Z, PAD_R, PAD_A, PAD_L, PAD_B, PAD_DD, PAD_DU, PAD_DL, PAD_DR}};

// ---- button mapping (Options -> Controller Settings -> Button Mapping)
const char *bind_names[NUM_BINDS] = {"Fire", "Alternate fire", "Jump", "Duck", "Use", "Reload", "Flashlight", "Previous weapon", "Next weapon"};
const char *pad_names[NUM_PADS] = {"A", "B", "Z", "L", "R", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "C-up", "C-down", "C-left", "C-right",
                                   "Stick up", "Stick down", "Stick left", "Stick right", "X (GameCube)", "Y (GameCube)"};
// the stick as four buttons, for the schemes where it doesn't move or look (pushed past half way)
static uint8_t stick_held, stick_pressed;
void pad_stick_update(const joypad_inputs_t *in)
{
    uint8_t h = (in->stick_y > 45 ? 1 : 0) | (in->stick_y < -45 ? 2 : 0) | (in->stick_x < -45 ? 4 : 0) | (in->stick_x > 45 ? 8 : 0);
    stick_pressed = h & ~stick_held;
    stick_held = h;
}
// the scheme's own moving / looking inputs can't also be actions: an action on one of them moves to
// the same direction on the input the scheme leaves free (D-pad <-> C buttons <-> stick)
void settings_fit_binds(void)
{
    int s = settings.control_scheme;
    bool dpad_used = s == 3 || s == 4, c_used = s != 3, stick_used = s != 4;
    for (int i = 0; i < NUM_BINDS; i++) {
        int b = settings.bind[i];
        int grp = b >= PAD_DU && b <= PAD_DR ? 0 : b >= PAD_CU && b <= PAD_CR ? 1 : b >= PAD_SU && b <= PAD_SR ? 2 : -1;
        if (grp < 0) continue;
        int dir = b - (grp == 0 ? PAD_DU : grp == 1 ? PAD_CU : PAD_SU);
        bool used = grp == 0 ? dpad_used : grp == 1 ? c_used : stick_used;
        if (!used) continue;
        int to = !dpad_used ? PAD_DU : !c_used ? PAD_CU : !stick_used ? PAD_SU : -1;
        if (to >= 0) settings.bind[i] = to + dir;
    }
}
void settings_default_binds(void)
{
    static const uint8_t def[NUM_BINDS] = {PAD_Z, PAD_R, PAD_A, PAD_L, PAD_B, PAD_DD, PAD_DU, PAD_DL, PAD_DR};
    memcpy(settings.bind, def, sizeof(def));
}
bool pad_held(const joypad_inputs_t *in, int pad)
{
    switch (pad) {
    case PAD_A: return in->btn.a; case PAD_B: return in->btn.b; case PAD_Z: return in->btn.z;
    case PAD_L: return in->btn.l; case PAD_R: return in->btn.r;
    case PAD_DU: return in->btn.d_up; case PAD_DD: return in->btn.d_down; case PAD_DL: return in->btn.d_left; case PAD_DR: return in->btn.d_right;
    case PAD_CU: return in->btn.c_up; case PAD_CD: return in->btn.c_down; case PAD_CL: return in->btn.c_left; case PAD_CR: return in->btn.c_right;
    case PAD_SU: case PAD_SD: case PAD_SL: case PAD_SR: return (stick_held >> (pad - PAD_SU)) & 1;
    case PAD_X: return in->btn.x; case PAD_Y: return in->btn.y;
    }
    return false;
}
bool pad_pressed(const joypad_buttons_t *b, int pad)
{
    switch (pad) {
    case PAD_A: return b->a; case PAD_B: return b->b; case PAD_Z: return b->z;
    case PAD_L: return b->l; case PAD_R: return b->r;
    case PAD_DU: return b->d_up; case PAD_DD: return b->d_down; case PAD_DL: return b->d_left; case PAD_DR: return b->d_right;
    case PAD_CU: return b->c_up; case PAD_CD: return b->c_down; case PAD_CL: return b->c_left; case PAD_CR: return b->c_right;
    case PAD_SU: case PAD_SD: case PAD_SL: case PAD_SR: return (stick_pressed >> (pad - PAD_SU)) & 1;
    case PAD_X: return b->x; case PAD_Y: return b->y;
    }
    return false;
}
// settings live in the last 2 KB of the 32 KB SRAM (the three save slots use 30 KB)
#define SETTINGS_SRAM_OFS (3 * 10240)
#define SETTINGS_MAGIC 0x484C5331   // "HLS1"
typedef struct { uint32_t magic; settings_t s; } __attribute__((aligned(16))) settings_blob_t;
#define SETTINGS_PAK_FILE "cpak1:/NHLE.01-HL64 CONFIG"
bool game_pak_ready(void);
// on the cartridge's save memory and, when one is in, on the Controller Pak (to take to another console)
void settings_save(void)
{
    settings_blob_t b;
    memset(&b, 0, sizeof(b));
    b.magic = SETTINGS_MAGIC;
    b.s = settings;
    b.s.cheats = false;   // cheats are per session
    if (sram_detect() > 0) sram_write(&b, SETTINGS_SRAM_OFS, sizeof(b));
    if (game_pak_ready()) {
        FILE *f = fopen(SETTINGS_PAK_FILE, "wb");
        if (f) { fwrite(&b, 1, sizeof(b), f); fclose(f); }
    }
}
void settings_load(void)
{
    sram_init();
    settings_blob_t b;
    bool got = false;
    if (sram_detect() > 0) { sram_read(&b, SETTINGS_SRAM_OFS, sizeof(b)); got = b.magic == SETTINGS_MAGIC; }
    if (!got && game_pak_ready()) {   // a new cartridge (or one without save memory): the pak's copy
        FILE *f = fopen(SETTINGS_PAK_FILE, "rb");
        if (f) { got = fread(&b, 1, sizeof(b), f) == sizeof(b) && b.magic == SETTINGS_MAGIC; fclose(f); }
    }
    if (!got) return;
    bool cheats = settings.cheats;
    settings = b.s;
    settings.cheats = cheats;
    for (int i = 0; i < NUM_BINDS; i++) if (settings.bind[i] >= NUM_PADS) { settings_default_binds(); break; }
    if (settings.control_scheme < 0 || settings.control_scheme >= NUM_SCHEMES) settings.control_scheme = 0;
    if (settings.look_sens < 1 || settings.look_sens > 10) settings.look_sens = 5;
    if (settings.volume < 0 || settings.volume > 10) settings.volume = 8;
}

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

typedef enum { SCR_MAIN, SCR_NEWGAME, SCR_LOAD, SCR_SAVE, SCR_OPTIONS, SCR_OPTIONS_LEGACY, SCR_OPTIONS_CTRL, SCR_OPTIONS_CTRL_ADV, SCR_OPTIONS_BINDS, SCR_OPTIONS_AV, SCR_OPTIONS_CREDITS, SCR_OPTIONS_CHEATS, SCR_LEAVE } screen_t;

static bool open, in_game;
static screen_t screen;
static int bind_sel;
static int cheat_sel;
static int cheat_count(void);
static bool bind_listen;
static float bind_listen_t;
static int main_sel, new_sel = 2, load_sel, load_scroll, opt_tab, opt_sel, opt_menu_sel, ctrl_sel, adv_sel, av_sel, leave_sel;
static float anim_t, repeat_t;
static float menu_time;
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
    int h = 16;
    w = (int)(w * 1.02f + 0.5f);
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
void game_save_to_slot(int slot);
bool game_save_slot_info(int slot, char *map_out, int map_len);
int game_saves_list(int *slots, char (*labels)[48]);
static int pak_sel;
// the Controller Pak column: is one in, and its saves (slots 3..5, see main.c)
static bool pak_ok;
static int pak_slots[3], num_pak_saves;
bool game_pak_ready(void);
int game_pak_saves_list(int *slots);
void game_saves_scan(void);
static void pak_refresh(void) { pak_ok = game_pak_ready(); game_saves_scan(); num_pak_saves = pak_ok ? game_pak_saves_list(pak_slots) : 0; }
static sprite_t *colorstrip2_spr;
static bool colorstrip2_tried;
static int num_saves, save_slots[6];
static char save_labels[6][48];
typedef enum { MI_CONTINUE, MI_RESUME, MI_SAVE, MI_NEWGAME, MI_LOAD, MI_OPTIONS, MI_LEAVE } mitem_t;
static const char *mi_label[] = {"Continue", "Resume game", "Save game", "New game", "Load game", "Options", "Leave game"};
static const char *mi_hint[] = {"Continue from your most recent save.", "", "Save your progress here.", "Start a new single player game.", "Load an autosave or a saved game.",
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
static const char *scheme_names[NUM_SCHEMES] = {"Stick moves, C looks", "Stick looks, C moves", "Stick moves + turns", "D-pad moves, stick looks", "D-pad moves, C looks"};
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
        o[n++] = (opt_t){"Control scheme", &settings.control_scheme, NULL, 0, NUM_SCHEMES - 1, scheme_names};
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
    menu_time += dt;
    if (!open) return MENU_ACT_NONE;
    mitem_t items[6];
    int n = main_items(items);
    switch (screen) {
    case SCR_MAIN: {
        int d = nav_dir(dt, in, pressed, false);
        if (d) {
            main_sel = (main_sel + d + n) % n;
            snd_play("ui/select.wav", 1.0f, 3);
        }
        if (pressed.start && in_game) { open = false; return MENU_ACT_RESUME; }
        if (pressed.b && in_game) { open = false; return MENU_ACT_RESUME; }
        if (pressed.a || pressed.start) {
            snd_play("ui/confirm.wav", 1.0f, 3);
            switch (items[main_sel]) {
            case MI_RESUME: open = false; return MENU_ACT_RESUME;
            case MI_CONTINUE: open = false; strncpy(map_out, "*save", map_out_len); return MENU_ACT_START_MAP;
            case MI_NEWGAME: screen = SCR_NEWGAME; new_sel = 0; break;
            case MI_LOAD: screen = SCR_LOAD; pak_sel = 0; num_saves = game_saves_list(save_slots, save_labels); load_sel = load_scroll = 0; break;
            case MI_SAVE: screen = SCR_SAVE; pak_sel = 0; num_saves = game_saves_list(save_slots, save_labels); load_sel = load_scroll = 0; break;
            case MI_OPTIONS: screen = SCR_OPTIONS; opt_menu_sel = 0; anim_t = 0.0f; break;
            case MI_LEAVE: screen = SCR_LEAVE; leave_sel = 1; break;
            }
        }
        break;
    }
    case SCR_NEWGAME: {
        int d = nav_dir(dt, in, pressed, false);
        if (d) {
            new_sel = (new_sel + d + 4) % 4;
            snd_play("ui/select.wav", 1.0f, 3);
        }
        if (pressed.b) {
            snd_play("ui/cancel.wav", 1.0f, 3);
            screen = SCR_MAIN;
            break;
        }
        if (pressed.a || pressed.start) {
            snd_play("ui/confirm.wav", 1.0f, 3);
            open = false;
            if (new_sel == 3) strncpy(map_out, "t0a0", map_out_len);
            else { settings.skill = new_sel + 1; strncpy(map_out, "c0a0", map_out_len); }
            return MENU_ACT_START_MAP;
        }
        break;
    }
    case SCR_SAVE:
    case SCR_LOAD: {
        bool is_save = (screen == SCR_SAVE);
        int lr = (pressed.d_right || in.stick_x > 30) - (pressed.d_left || in.stick_x < -30);
        if (lr != 0) {
            int old_pak = pak_sel;
            if (lr > 0) pak_sel = 1;
            else pak_sel = 0;
            if (pak_sel != old_pak) {
                load_sel = 0;
                load_scroll = 0;
                if (pak_sel == 1) pak_refresh();
                snd_play("ui/select.wav", 1.0f, 3);
            }
        }

        int total = 1;
        if (pak_sel == 0) {
            total = is_save ? 2 : num_saves;   // load: autosaves and saved games only
        } else {
            total = !pak_ok ? 0 : is_save ? 3 : num_pak_saves;
        }
        if (!total) {
            if (pressed.b) { snd_play("ui/cancel.wav", 1.0f, 3); screen = SCR_MAIN; }
            if (pressed.a && pak_sel == 1) { pak_refresh(); snd_play("ui/select.wav", 1.0f, 3); }   // check for a pak again
            else if (pressed.a) screen = SCR_MAIN;
            break;
        }

        int d = nav_dir(dt, in, pressed, false);
        if (d) {
            load_sel = (load_sel + d + total) % total;
            snd_play("ui/select.wav", 1.0f, 3);
        }
        if (load_sel < load_scroll) load_scroll = load_sel;
        if (load_sel >= load_scroll + 6) load_scroll = load_sel - 5;

        if (pressed.b) {
            snd_play("ui/cancel.wav", 1.0f, 3);
            screen = SCR_MAIN;
            break;
        }

        if (pressed.a || pressed.start) {
            if (is_save) {
                snd_play("ui/confirm.wav", 1.0f, 3);
                open = false;
                game_save_to_slot(pak_sel == 0 ? load_sel + 1 : 3 + load_sel);
                return MENU_ACT_RESUME;
            } else {
                snd_play("ui/confirm.wav", 1.0f, 3);
                open = false;
                snprintf(map_out, map_out_len, "*save%d", pak_sel == 0 ? save_slots[load_sel] : pak_slots[load_sel]);
                return MENU_ACT_START_MAP;
            }
        }
        break;
    }
    case SCR_OPTIONS: {
        int d = nav_dir(dt, in, pressed, false);
        if (d) {
            opt_menu_sel = (opt_menu_sel + d + 4) % 4;
            snd_play("ui/select.wav", 1.0f, 3);
        }
        if (pressed.b) {
            snd_play("ui/cancel.wav", 1.0f, 3);
            screen = SCR_MAIN;
            settings_save();   // control scheme, sensitivity, volume, button mapping: kept on the cartridge
            break;
        }
        if (pressed.a || pressed.start) {
            snd_play("ui/confirm.wav", 1.0f, 3);
            switch (opt_menu_sel) {
            case 0: // Audio / Video Settings
                screen = SCR_OPTIONS_AV;
                av_sel = 0;
                break;
            case 1: // Controller Settings
                screen = SCR_OPTIONS_CTRL;
                ctrl_sel = 0;
                break;
            case 2: // Cheat Codes: only with "Enable cheats" ticked (Controller Settings > Advanced)
                if (!settings.cheats) { snd_play("ui/cancel.wav", 1.0f, 3); break; }
                screen = SCR_OPTIONS_CHEATS;
                cheat_sel = 0;
                break;
            case 3: // Credits
                screen = SCR_OPTIONS_CREDITS;
                break;
            }
        }
        break;
    }
    case SCR_OPTIONS_CTRL: {
        int d = nav_dir(dt, in, pressed, false);
        if (d) {
            ctrl_sel = (ctrl_sel + d + 3) % 3;
            snd_play("ui/select.wav", 1.0f, 3);
        }
        if (pressed.b) {
            snd_play("ui/cancel.wav", 1.0f, 3);
            screen = SCR_OPTIONS;
            break;
        }
        int lr = 0;
        if (pressed.d_left || (in.stick_x < -30 && fabs(in.stick_x) > fabs(in.stick_y))) lr = -1;
        if (pressed.d_right || (in.stick_x > 30 && fabs(in.stick_x) > fabs(in.stick_y))) lr = 1;
        if (lr != 0) {
            repeat_t += dt;
            if (repeat_dir != lr) { repeat_dir = lr; repeat_t = 0; }
            else if (repeat_t < 0.25f) lr = 0;
            else repeat_t = 0.15f;
        } else {
            repeat_dir = 0;
            repeat_t = 0;
        }

        if (ctrl_sel == 0) { // Control Scheme
            if (lr < 0) {
                if (settings.control_scheme > 0) {
                    settings.control_scheme--;
                    settings_fit_binds();
                    snd_play("ui/select.wav", 1.0f, 3);
                }
            } else if (lr > 0) {
                if (settings.control_scheme < NUM_SCHEMES - 1) {
                    settings.control_scheme++;
                    settings_fit_binds();
                    snd_play("ui/confirm.wav", 1.0f, 3);
                }
            } else if (pressed.a || pressed.start) {
                if (settings.control_scheme < NUM_SCHEMES - 1) {
                    settings.control_scheme++;
                } else {
                    settings.control_scheme = 0;
                }
                settings_fit_binds();
                snd_play("ui/confirm.wav", 1.0f, 3);
            }
        } else if (ctrl_sel == 1) { // Advanced...
            if (lr < 0) {
                ctrl_sel = 0;
                snd_play("ui/select.wav", 1.0f, 3);
            } else if (pressed.a || pressed.start) {
                snd_play("ui/confirm.wav", 1.0f, 3);
                screen = SCR_OPTIONS_CTRL_ADV;
                adv_sel = 0;
            }
        } else if (ctrl_sel == 2) { // Button Mapping...
            if (pressed.a || pressed.start) {
                snd_play("ui/confirm.wav", 1.0f, 3);
                screen = SCR_OPTIONS_BINDS;
                bind_sel = 0;
                bind_listen = false;
            }
        }
        break;
    }
    case SCR_OPTIONS_BINDS: {
        if (bind_listen) {
            // the next button pressed becomes this action's (Start cancels); the action that had
            // that button takes this one's old button, so nothing is left unbound
            bind_listen_t += dt;
            if (pressed.start) { bind_listen = false; snd_play("ui/cancel.wav", 1.0f, 3); break; }
            if (bind_listen_t < 0.2f) break;
            pad_stick_update(&in);
            for (int p = 0; p < NUM_PADS; p++) {
                if (!pad_pressed(&pressed, p)) continue;
                {   // not one the control scheme moves or looks with
                    int sc = settings.control_scheme;
                    bool dpad_used = sc == 3 || sc == 4, c_used = sc != 3, stick_used = sc != 4;
                    if ((p >= PAD_DU && p <= PAD_DR && dpad_used) || (p >= PAD_CU && p <= PAD_CR && c_used) ||
                        (p >= PAD_SU && p <= PAD_SR && stick_used)) continue;
                }
                int old = settings.bind[bind_sel];
                for (int j = 0; j < NUM_BINDS; j++) if (j != bind_sel && settings.bind[j] == p) settings.bind[j] = old;
                settings.bind[bind_sel] = p;
                bind_listen = false;
                snd_play("ui/confirm.wav", 1.0f, 3);
                break;
            }
            break;
        }
        int d = nav_dir(dt, in, pressed, false);
        if (d) { bind_sel = (bind_sel + d + NUM_BINDS + 1) % (NUM_BINDS + 1); snd_play("ui/select.wav", 1.0f, 3); }
        if (pressed.b) { snd_play("ui/cancel.wav", 1.0f, 3); screen = SCR_OPTIONS_CTRL; settings_save(); break; }
        if (pressed.a) {
            snd_play("ui/confirm.wav", 1.0f, 3);
            if (bind_sel == NUM_BINDS) { settings_default_binds(); settings_fit_binds(); }
            else { bind_listen = true; bind_listen_t = 0; }
        }
        break;
    }
    case SCR_OPTIONS_CTRL_ADV: {
        int d = nav_dir(dt, in, pressed, false);
        if (d) {
            adv_sel = (adv_sel + d + 3) % 3;
            snd_play("ui/select.wav", 1.0f, 3);
        }
        if (pressed.b) {
            snd_play("ui/cancel.wav", 1.0f, 3);
            screen = SCR_OPTIONS_CTRL;
            break;
        }
        int lr = 0;
        if (pressed.d_left || (in.stick_x < -30 && fabs(in.stick_x) > fabs(in.stick_y))) lr = -1;
        if (pressed.d_right || (in.stick_x > 30 && fabs(in.stick_x) > fabs(in.stick_y))) lr = 1;
        if (lr != 0) {
            repeat_t += dt;
            if (repeat_dir != lr) { repeat_dir = lr; repeat_t = 0; }
            else if (repeat_t < 0.25f) lr = 0;
            else repeat_t = 0.15f;
        } else {
            repeat_dir = 0;
            repeat_t = 0;
        }

        switch (adv_sel) {
        case 0: // Look Sensitivity
            if (lr != 0) {
                int ns = settings.look_sens + lr;
                if (ns >= 1 && ns <= 10) {
                    settings.look_sens = ns;
                    snd_play("ui/select.wav", 1.0f, 3);
                }
            }
            break;
        case 1: // Reverse up-down axis
            if (pressed.a || pressed.start || lr != 0) {
                settings.invert_y = !settings.invert_y;
                snd_play("ui/confirm.wav", 1.0f, 3);
            }
            break;
        case 2: // Enable Cheats
            if (pressed.a || pressed.start || lr != 0) {
                settings.cheats = !settings.cheats;
                snd_play("ui/confirm.wav", 1.0f, 3);
            }
            break;
        }
        break;
    }
    case SCR_OPTIONS_AV: {
        int d = nav_dir(dt, in, pressed, false);
        if (d) {
            av_sel = (av_sel + d + 2) % 2;
            snd_play("ui/select.wav", 1.0f, 3);
        }
        if (pressed.b) {
            snd_play("ui/cancel.wav", 1.0f, 3);
            screen = SCR_OPTIONS;
            break;
        }
        int lr = 0;
        if (pressed.d_left || (in.stick_x < -30 && fabs(in.stick_x) > fabs(in.stick_y))) lr = -1;
        if (pressed.d_right || (in.stick_x > 30 && fabs(in.stick_x) > fabs(in.stick_y))) lr = 1;
        if (lr != 0) {
            repeat_t += dt;
            if (repeat_dir != lr) { repeat_dir = lr; repeat_t = 0; }
            else if (repeat_t < 0.25f) lr = 0;
            else repeat_t = 0.15f;
        } else {
            repeat_dir = 0;
            repeat_t = 0;
        }

        switch (av_sel) {
        case 0: // Game volume
            if (lr != 0) {
                int nv = settings.volume + lr;
                if (nv >= 0 && nv <= 10) {
                    settings.volume = nv;
                    snd_set_volume(settings.volume / 10.0f);
                    snd_play("ui/select.wav", 1.0f, 3);
                }
            }
            break;
        case 1: // Show frame rate
            if (pressed.a || pressed.start || lr != 0) {
                settings.show_stats = !settings.show_stats;
                snd_play("ui/confirm.wav", 1.0f, 3);
            }
            break;
        }
        break;
    }
    case SCR_OPTIONS_CHEATS: {
        int nc = cheat_count();
        int d = nav_dir(dt, in, pressed, false);
        if (d) { cheat_sel = (cheat_sel + d + nc) % nc; snd_play("ui/select.wav", 1.0f, 3); }
        if (pressed.b) { snd_play("ui/cancel.wav", 1.0f, 3); screen = SCR_OPTIONS; break; }
        int h = nav_dir(dt, in, pressed, true);
        if (cheat_sel == 3 && h) {   // map: left / right
            int nm = game_num_maps();
            dbg_map = (dbg_map + h + nm) % nm;
            snd_play("ui/select.wav", 1.0f, 3);
        }
        if (pressed.a || pressed.start || (h && cheat_sel < 2)) {
            switch (cheat_sel) {
            case 0: dbg_god = !dbg_god; snd_play("ui/confirm.wav", 1.0f, 3); break;
            case 1: dbg_noclip = !dbg_noclip; snd_play("ui/confirm.wav", 1.0f, 3); break;
            case 2:
                if (!pressed.a && !pressed.start) break;
                if (in_game) { dbg_give_all = true; snd_play("ui/confirm.wav", 1.0f, 3); open = false; return MENU_ACT_RESUME; }
                snd_play("ui/cancel.wav", 1.0f, 3);   // nothing to give on the title screen
                break;
            case 3: case 4:
                if (!pressed.a && !pressed.start) break;
                snd_play("ui/confirm.wav", 1.0f, 3);
                warped = true;
                open = false;
                strncpy(map_out, game_map_name(dbg_map), map_out_len);
                return MENU_ACT_START_MAP;
            }
        }
        break;
    }
    case SCR_OPTIONS_CREDITS: {
        if (pressed.b || pressed.a || pressed.start) {
            snd_play("ui/cancel.wav", 1.0f, 3);
            screen = SCR_OPTIONS;
        }
        break;
    }
    case SCR_OPTIONS_LEGACY: {
        opt_t o[8];
        int no = options_for_tab(opt_tab, o);
        if (pressed.r || pressed.z) {
            int nt = settings.cheats ? 4 : 3;
            opt_tab = (opt_tab + (pressed.r ? 1 : nt - 1)) % nt;
            opt_sel = 0;
            snd_play("ui/select.wav", 1.0f, 3);
            break;
        }
        int d = nav_dir(dt, in, pressed, false);
        if (d) {
            opt_sel = (opt_sel + d + no) % no;
            snd_play("ui/select.wav", 1.0f, 3);
        }
        int h = nav_dir(dt, in, pressed, true);
        if (h) {
            option_change(&o[opt_sel], h);
            snd_play("ui/select.wav", 1.0f, 3);
        }
        if (pressed.a) {
            option_change(&o[opt_sel], 1);
            snd_play("ui/confirm.wav", 1.0f, 3);
        }
        if (dbg_warp) {   // Debug: load the chosen map at its start point
            dbg_warp = false;
            warped = true;
            open = false;
            strncpy(map_out, game_map_name(dbg_map), map_out_len);
            return MENU_ACT_START_MAP;
        }
        if (dbg_give_all) { open = false; return MENU_ACT_RESUME; }
        if (pressed.b || pressed.start) {
            snd_play("ui/cancel.wav", 1.0f, 3);
            screen = SCR_OPTIONS;
        }
        break;
    }
    case SCR_LEAVE: {
        int h = nav_dir(dt, in, pressed, true);
        if (h) {
            leave_sel ^= 1;
            snd_play("ui/select.wav", 1.0f, 3);
        }
        if (pressed.b) {
            snd_play("ui/cancel.wav", 1.0f, 3);
            screen = SCR_MAIN;
        }
        if (pressed.a) {
            snd_play("ui/confirm.wav", 1.0f, 3);
            if (leave_sel == 0) { in_game = false; screen = SCR_MAIN; main_sel = 0; return MENU_ACT_LEAVE; }
            screen = SCR_MAIN;
        }
        break;
    }
    }
    return MENU_ACT_NONE;
}

// ------------------------------------------------------------------ drawing
static sprite_t *glowcircle_spr;
static bool glowcircle_tried;
static sprite_t *buttons_a_spr;
static bool buttons_a_tried;
static sprite_t *buttons_b_spr;
static bool buttons_b_tried;
static sprite_t *color2_spr;
static bool color2_tried;
static sprite_t *ctrl_spr;
static bool ctrl_tried;
static sprite_t *slider_spr;
static bool slider_tried;
static sprite_t *slider_end_cap_spr;
static bool slider_end_cap_tried;
static sprite_t *slider_piece_spr;
static bool slider_piece_tried;
static sprite_t *box_spr;
static bool box_tried;
static sprite_t *hl_top_spr;
static bool hl_top_tried;
static sprite_t *hl_bot_spr;
static bool hl_bot_tried;

// Ping-pong / bounce trajectory: constant speed, hard stop and instant reversal at boundaries
static float bounce_pos(float time, float min_x, float max_x, float speed)
{
    float span = max_x - min_x;
    if (span <= 0.0f) return min_x;
    float cycle = 2.0f * span;
    float p = fmodf(time * speed, cycle);
    if (p < 0.0f) p += cycle;
    return (p <= span) ? (min_x + p) : (max_x - (p - span));
}

static void draw_title_hl(void)
{
    if (in_game) return;
    if (!hl_top_tried) {
        hl_top_tried = true;
        FILE *f = fopen("rom:/ui/hl_top.sprite", "rb");
        if (f) {
            fclose(f);
            hl_top_spr = sprite_load("rom:/ui/hl_top.sprite");
        }
    }
    if (!hl_bot_tried) {
        hl_bot_tried = true;
        FILE *f = fopen("rom:/ui/hl_bot.sprite", "rb");
        if (f) {
            fclose(f);
            hl_bot_spr = sprite_load("rom:/ui/hl_bot.sprite");
        }
    }
    if (!hl_top_spr && !hl_bot_spr) {
        if (logo.spr) {
            font_begin(BLEND_ALPHA);
            font_color(RGBA32(255, 255, 255, 255));
            image_draw(&logo, (SW - logo.surf.width) / 2, 44, 0, 0, logo.surf.width, logo.surf.height);
        }
        return;
    }
    float scale_bot = 0.48f; // Much bigger foreground title
    float scale_top = 0.75f; // Large ambient background glow
    float offset_5pct = SW * 0.05f;

    // 1. Top part of asset: rendered under / behind bottom part, 50% opacity, slightly slower bounce speed
    if (hl_top_spr) {
        float cx_top = (SW - hl_top_spr->width * scale_top) * 0.5f;
        float x_top = bounce_pos(menu_time, cx_top - offset_5pct, cx_top + offset_5pct, 6.0f);
        int y_top = (int)(SH * 0.10f) + 6; // slightly lower under the bottom part

        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, TEX0), (TEX0, 0, PRIM, 0)));
        rdpq_set_prim_color(RGBA32(255, 255, 255, 128)); // 50% opacity
        rdpq_mode_filter(FILTER_BILINEAR);

        rdpq_sprite_blit(hl_top_spr, x_top, y_top, &(rdpq_blitparms_t){
            .scale_x = scale_top,
            .scale_y = scale_top,
        });
    }

    // 2. Bottom part of asset: foreground title, offset from top of screen by 10%, constant trajectory bounce
    if (hl_bot_spr) {
        float cx_bot = (SW - hl_bot_spr->width * scale_bot) * 0.5f;
        float x_bot = bounce_pos(menu_time, cx_bot - offset_5pct, cx_bot + offset_5pct, 8.0f);
        int y_bot = (int)(SH * 0.10f); // offset by the top of the screen by 10%

        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_filter(FILTER_BILINEAR);

        rdpq_sprite_blit(hl_bot_spr, x_bot, y_bot, &(rdpq_blitparms_t){
            .scale_x = scale_bot,
            .scale_y = scale_bot,
        });
    }
}

// the title art's arc: menu items sit along it, so each item's indent follows from its height
// (the Options list's spacing: 106 -> 2%, 126 -> 3%, 146 -> 5%, 166 -> 9% of the screen width)
static int arc_x(int y)
{
    static const float py[] = {86, 106, 126, 146, 166, 186, 206, 226, 246};
    static const float px[] = {5.5f, 6.4f, 9.6f, 16.0f, 28.8f, 46.0f, 68.0f, 95.0f, 128.0f};
    if (y <= py[0]) return (int)px[0];
    for (int i = 1; i < 9; i++)
        if (y <= py[i]) return (int)(px[i - 1] + (px[i] - px[i - 1]) * (y - py[i - 1]) / (py[i] - py[i - 1]) + 0.5f);
    return (int)px[8];
}

static void draw_main(void)
{
    draw_title_hl();
    if (!glowcircle_tried) {
        glowcircle_tried = true;
        glowcircle_spr = sprite_load("rom:/ui/glowcircle.sprite");
    }
    if (!buttons_a_tried) {
        buttons_a_tried = true;
        buttons_a_spr = sprite_load("rom:/ui/buttons_a.sprite");
    }
    if (!buttons_b_tried) {
        buttons_b_tried = true;
        FILE *f = fopen("rom:/ui/buttons_b.sprite", "rb");
        if (f) { fclose(f); buttons_b_spr = sprite_load("rom:/ui/buttons_b.sprite"); }
    }
    if (!colorstrip2_tried) {
        colorstrip2_tried = true;
        FILE *f = fopen("rom:/ui/colorstrip2.sprite", "rb");
        if (f) { fclose(f); colorstrip2_spr = sprite_load("rom:/ui/colorstrip2.sprite"); }
    }

    mitem_t items[6];
    int n = main_items(items);
    bool in_newgame = (screen == SCR_NEWGAME);
    int x = 36;
    int y = in_newgame ? 72 : 112;
    int step = 21;
    int sub_step = 18;

    static const char *diff_opts[] = {"Easy", "Normal", "Difficult", "Training Room"};

    int y_cursor = y;

    for (int i = 0; i < n; i++) {
        int item_x = x + arc_x(y_cursor);

        bool is_newgame_item = (items[i] == MI_NEWGAME);
        bool armed = false;
        if (in_newgame) {
            armed = is_newgame_item;
        } else if (screen == SCR_MAIN) {
            armed = (i == main_sel);
        }

        bool faded = in_newgame && !is_newgame_item;

        if (screen == SCR_MAIN && anim_t < 0.06f * i) {
            y_cursor += step;
            continue;
        }

        int item_y = y_cursor;

        // Drop shadow for menu item
        font_begin(BLEND_ALPHA);
        rdpq_mode_filter(FILTER_BILINEAR);
        font_color(faded ? RGBA32(0, 0, 0, 43) : RGBA32(0, 0, 0, 217));
        font_draw_scaled(FONT_MEDIUM, item_x + 2, item_y + 2, mi_label[items[i]], 1.10f);

        if (armed && !faded) {
            if (glowcircle_spr) {
                rdpq_set_mode_standard();
                rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
                rdpq_mode_filter(FILTER_BILINEAR);
                float gc_scale = 0.70f;
                int fh = (int)(font_height(FONT_MEDIUM) * 1.10f);
                int gx = item_x - 24;
                int gy = item_y + (fh - (int)(glowcircle_spr->height * gc_scale)) / 2;
                rdpq_sprite_blit(glowcircle_spr, gx, gy, &(rdpq_blitparms_t){ .scale_x = gc_scale, .scale_y = gc_scale });
            }
            float pulse = 0.75f + 0.25f * sinf(anim_t * 4.0f);
            font_begin(BLEND_ADD);
            rdpq_mode_filter(FILTER_BILINEAR);
            font_color(RGBA32(255 * pulse * 0.55f, 178 * pulse * 0.55f, 67 * pulse * 0.55f, 255));
            font_draw_scaled(FONT_GLOW, item_x, item_y, mi_label[items[i]], 1.10f);
        }

        font_begin(BLEND_ALPHA);
        rdpq_mode_filter(FILTER_BILINEAR);
        if (faded) {
            font_color(RGBA32(196, 181, 80, 51)); // 20% opacity
        } else {
            font_color(armed ? C_MENU_ARMED : C_MENU);
        }
        font_draw_scaled(FONT_MEDIUM, item_x, item_y, mi_label[items[i]], 1.10f);

        y_cursor += step;

        // When in New Game, expand the difficulty + training room options right below New game
        if (in_newgame && is_newgame_item) {
            int sub_x = item_x + 36;
            for (int k = 0; k < 4; k++) {
                int ry = y_cursor + k * sub_step;
                bool sub_sel = (k == new_sel);

                if (sub_sel && colorstrip2_spr) {
                    int strip_x = sub_x - 10;
                    int strip_w = SW - strip_x - 16;
                    int strip_h = 18;
                    rdpq_set_mode_standard();
                    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
                    rdpq_mode_filter(FILTER_BILINEAR);
                    rdpq_sprite_blit(colorstrip2_spr, strip_x, ry, &(rdpq_blitparms_t){
                        .scale_x = (float)strip_w / colorstrip2_spr->width,
                        .scale_y = (float)strip_h / colorstrip2_spr->height,
                    });
                }

                font_begin(BLEND_ALPHA);
                rdpq_mode_filter(FILTER_BILINEAR);
                font_color(RGBA32(0, 0, 0, 217));
                font_draw_scaled(FONT_MEDIUM, sub_x + 1, ry + 2 + 1, diff_opts[k], 1.05f);
                font_color(sub_sel ? RGBA32(235, 195, 65, 255) : RGBA32(178, 122, 32, 255));
                font_draw_scaled(FONT_MEDIUM, sub_x, ry + 2, diff_opts[k], 1.05f);
            }
            y_cursor += 4 * sub_step + 3;
        }
    }

    if (in_newgame) {
        int hy = SH - 22;
        float btn_scale = 0.459f;
        int fh = font_height(FONT_SMALL);
        if (buttons_a_spr) {
            int by = hy + (fh - (int)(buttons_a_spr->height * btn_scale)) / 2 - 2;
            rdpq_set_mode_standard();
            rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
            rdpq_mode_filter(FILTER_BILINEAR);
            rdpq_sprite_blit(buttons_a_spr, x, by, &(rdpq_blitparms_t){ .scale_x = btn_scale, .scale_y = btn_scale });
        }
        font_begin(BLEND_ALPHA);
        font_color(RGBA32(0, 0, 0, 217));
        font_draw(FONT_SMALL, x + 18 + 1, hy + 1, "Play");
        font_color(C_MENU);
        font_draw(FONT_SMALL, x + 18, hy, "Play");

        if (buttons_b_spr) {
            int by = hy + (fh - (int)(buttons_b_spr->height * btn_scale)) / 2 - 2;
            rdpq_set_mode_standard();
            rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
            rdpq_mode_filter(FILTER_BILINEAR);
            rdpq_sprite_blit(buttons_b_spr, x + 90, by, &(rdpq_blitparms_t){ .scale_x = btn_scale, .scale_y = btn_scale });
        }
        font_begin(BLEND_ALPHA);
        font_color(RGBA32(0, 0, 0, 217));
        font_draw(FONT_SMALL, x + 108 + 1, hy + 1, "Back");
        font_color(C_MENU);
        font_draw(FONT_SMALL, x + 108, hy, "Back");
    } else if (screen == SCR_MAIN) {
        int hy = SH - 30;
        int text_x = x;
        if (buttons_a_spr) {
            float btn_scale = 0.459f;
            int fh = font_height(FONT_SMALL);
            int by = hy + (fh - (int)(buttons_a_spr->height * btn_scale)) / 2 - 2;
            rdpq_set_mode_standard();
            rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
            rdpq_mode_filter(FILTER_BILINEAR);
            rdpq_sprite_blit(buttons_a_spr, x, by, &(rdpq_blitparms_t){ .scale_x = btn_scale, .scale_y = btn_scale });
            text_x = x + 18;
        }
        font_begin(BLEND_ALPHA);
        font_color(RGBA32(0, 0, 0, 217));
        font_draw(FONT_SMALL, text_x + 1, hy + 1, mi_hint[items[main_sel]]);
        font_begin(BLEND_ALPHA);
        font_color(C_MENU);
        font_draw(FONT_SMALL, text_x, hy, mi_hint[items[main_sel]]);
    }
}

static void to_title_case(char *dst, const char *src, int maxlen)
{
    bool cap = true;
    int j = 0;
    for (int i = 0; src[i] && j < maxlen - 1; i++) {
        char c = src[i];
        if (c == ' ' || c == '-' || c == '/' || c == '(' || c == '"') {
            dst[j++] = c;
            cap = true;
        } else if (cap) {
            dst[j++] = (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
            cap = false;
        } else {
            dst[j++] = (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
        }
    }
    dst[j] = 0;
}

static const char *get_chapter_title_for_map(const char *map)
{
    if (!map || !map[0]) return "";
    for (int i = 0; i < num_chapters; i++) {
        int ml = strlen(chapter_maps[i]);
        if (strncmp(map, chapter_maps[i], ml) == 0) {
            return chapters[i];
        }
    }
    for (int i = 0; i < num_chapters; i++) {
        if (strncmp(map, chapter_maps[i], 4) == 0) {
            return chapters[i];
        }
    }
    return map;
}

static void draw_load_save(bool is_save)
{
    draw_title_hl();

    if (!buttons_a_tried) {
        buttons_a_tried = true;
        FILE *f = fopen("rom:/ui/buttons_a.sprite", "rb");
        if (f) { fclose(f); buttons_a_spr = sprite_load("rom:/ui/buttons_a.sprite"); }
    }
    if (!buttons_b_tried) {
        buttons_b_tried = true;
        FILE *f = fopen("rom:/ui/buttons_b.sprite", "rb");
        if (f) { fclose(f); buttons_b_spr = sprite_load("rom:/ui/buttons_b.sprite"); }
    }
    if (!color2_tried) {
        color2_tried = true;
        FILE *f = fopen("rom:/ui/color2.sprite", "rb");
        if (f) { fclose(f); color2_spr = sprite_load("rom:/ui/color2.sprite"); }
    }
    if (!colorstrip2_tried) {
        colorstrip2_tried = true;
        FILE *f = fopen("rom:/ui/colorstrip2.sprite", "rb");
        if (f) { fclose(f); colorstrip2_spr = sprite_load("rom:/ui/colorstrip2.sprite"); }
    }

    const char *title_text = is_save ? "Save Game" : "Load Game";
    font_begin(BLEND_ALPHA);
    rdpq_mode_filter(FILTER_BILINEAR);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, 24 + 1, 56 + 1, title_text, 1.20f);
    font_color(RGBA32(235, 195, 65, 255));
    font_draw_scaled(FONT_MEDIUM, 24, 56, title_text, 1.20f);

    // Pak tabs
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, 24 + 1, 80 + 1, "Game Pak", 1.0f);
    font_color(pak_sel == 0 ? RGBA32(235, 195, 65, 255) : RGBA32(178, 122, 32, 255));
    font_draw_scaled(FONT_MEDIUM, 24, 80, "Game Pak", 1.0f);

    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, 124 + 1, 80 + 1, "Controller Pak", 1.0f);
    font_color(pak_sel == 1 ? RGBA32(235, 195, 65, 255) : RGBA32(178, 122, 32, 255));
    font_draw_scaled(FONT_MEDIUM, 124, 80, "Controller Pak", 1.0f);

    int cw = 280, ch = 114;
    int cx = (SW - cw) / 2;
    int cy = 98;

    rdpq_set_mode_standard();
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_filter(FILTER_BILINEAR);
    if (color2_spr) {
        rdpq_sprite_blit(color2_spr, cx, cy, &(rdpq_blitparms_t){
            .scale_x = (float)cw / color2_spr->width,
            .scale_y = (float)ch / color2_spr->height,
        });
    }

    // 1px amber border outline
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_set_prim_color(RGBA32(255, 178, 67, 180));
    rdpq_fill_rectangle(cx, cy, cx + cw, cy + 1);
    rdpq_fill_rectangle(cx, cy + ch - 1, cx + cw, cy + ch);
    rdpq_fill_rectangle(cx, cy, cx + 1, cy + ch);
    rdpq_fill_rectangle(cx + cw - 1, cy, cx + cw, cy + ch);

    int total = 1;
    if (pak_sel == 0) {
        total = is_save ? 2 : num_saves;   // load: autosaves and saved games only
    } else {
        total = !pak_ok || (!is_save && num_pak_saves == 0) ? 1 : is_save ? 3 : num_pak_saves;
    }

    for (int i = 0; i < 6 && load_scroll + i < total; i++) {
        int k = load_scroll + i;
        int ry = cy + 3 + i * 18;
        bool armed = (k == load_sel);

        if (armed && colorstrip2_spr) {
            rdpq_set_mode_standard();
            rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
            rdpq_mode_filter(FILTER_BILINEAR);
            rdpq_sprite_blit(colorstrip2_spr, cx + 1, ry, &(rdpq_blitparms_t){
                .scale_x = (float)(cw - 2) / colorstrip2_spr->width,
                .scale_y = 18.0f / colorstrip2_spr->height,
            });
        }

        char line[64];
        if (pak_sel == 1) {
            char mapname[32];
            int slot = is_save ? 3 + k : (num_pak_saves ? pak_slots[k] : -1);
            if (!pak_ok) strcpy(line, "No Controller Pak (A: look again)");
            else if (slot >= 0 && game_save_slot_info(slot, mapname, sizeof(mapname))) {
                char tc[48];
                to_title_case(tc, get_chapter_title_for_map(mapname), sizeof(tc));
                snprintf(line, sizeof(line), "Controller Pak %d - %s", slot - 2, tc);
            } else strcpy(line, is_save ? "-- Unused Data --" : "-- No saved games --");
        } else if (is_save) {
            char mapname[32];
            int slot = k + 1;
            if (game_save_slot_info(slot, mapname, sizeof(mapname))) {
                const char *cname = get_chapter_title_for_map(mapname);
                char tc[48];
                to_title_case(tc, cname, sizeof(tc));
                snprintf(line, sizeof(line), "Saved Game %d - %s", slot, tc);
            } else {
                strcpy(line, "-- Unused Data --");
            }
        } else {
            if (k < num_saves) {
                int slot = save_slots[k];
                const char *prefix = (slot == 0) ? "Autosave" : "Saved Game";
                char mapname[32] = "";
                game_save_slot_info(slot, mapname, sizeof(mapname));
                const char *cname = get_chapter_title_for_map(mapname);
                char tc[48];
                to_title_case(tc, cname, sizeof(tc));
                snprintf(line, sizeof(line), "%s - %s", prefix, tc);
            } else {
                to_title_case(line, chapters[k - num_saves], sizeof(line));
            }
        }

        int tx = cx + 8;
        int ty = ry + 2;

        font_begin(BLEND_ALPHA);
        rdpq_mode_filter(FILTER_BILINEAR);
        font_color(RGBA32(0, 0, 0, 217));
        font_draw(FONT_MEDIUM, tx + 1, ty + 1, line);
        font_color(armed ? RGBA32(235, 195, 65, 255) : RGBA32(178, 122, 32, 255));
        font_draw(FONT_MEDIUM, tx, ty, line);
    }

    if (total > 6) {
        int track = 6 * 18;
        int th = track * 6 / total;
        if (th < 6) th = 6;
        int ty = track * load_scroll / total;
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_set_prim_color(RGBA32(255, 178, 67, 180));
        rdpq_fill_rectangle(cx + cw - 4, cy + 3 + ty, cx + cw - 2, cy + 3 + ty + th);
    }

    int hy = SH - 22;
    float btn_scale = 0.459f;
    int by_btn = hy + (font_height(FONT_SMALL) - (int)(16 * btn_scale)) / 2 - 2;

    if (buttons_a_spr) {
        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_sprite_blit(buttons_a_spr, 24, by_btn, &(rdpq_blitparms_t){ .scale_x = btn_scale, .scale_y = btn_scale });
    }
    font_begin(BLEND_ALPHA);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw(FONT_SMALL, 42 + 1, hy + 1, is_save ? "Save game" : "Load game");
    font_color(C_MENU);
    font_draw(FONT_SMALL, 42, hy, is_save ? "Save game" : "Load game");

    if (buttons_b_spr) {
        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_sprite_blit(buttons_b_spr, 130, by_btn, &(rdpq_blitparms_t){ .scale_x = btn_scale, .scale_y = btn_scale });
    }
    font_begin(BLEND_ALPHA);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw(FONT_SMALL, 148 + 1, hy + 1, "Back");
    font_color(C_MENU);
    font_draw(FONT_SMALL, 148, hy, "Back");
}

static void draw_load(void)
{
    draw_load_save(false);
}

static void draw_save(void)
{
    draw_load_save(true);
}

static void draw_options(void)
{
    draw_title_hl();
    static const char *opt_labels[] = {
        "Audio / Video Settings",
        "Controller Settings",
        "Cheat Codes",
        "Credits"
    };
    static const char *opt_hints[] = {
        "Configure sound volume, video and display options.",
        "Configure control schemes, sensitivity and look axes.",
        "Toggle cheats and developer options (needs Enable cheats).",
        "View the team and contributors who made this possible."
    };
    int n = 4;
    int x = 36, y = 106, step = 20;
    for (int i = 0; i < n; i++) {
        int item_x = x + arc_x(y + i * step);
        bool armed = (i == opt_menu_sel && screen == SCR_OPTIONS);
        if (anim_t < 0.06f * i) continue;

        // Drop shadow for menu item (85% opacity, bottom-right offset)
        font_begin(BLEND_ALPHA);
        rdpq_mode_filter(FILTER_BILINEAR);
        font_color(RGBA32(0, 0, 0, 217));
        font_draw_scaled(FONT_MEDIUM, item_x + 2, y + i * step + 2, opt_labels[i], 1.10f);

        if (armed) {
            if (glowcircle_spr) {
                rdpq_set_mode_standard();
                rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
                rdpq_mode_filter(FILTER_BILINEAR);
                float gc_scale = 0.70f;
                int fh = (int)(font_height(FONT_MEDIUM) * 1.10f);
                int gx = item_x - 24;
                int gy = y + i * step + (fh - (int)(glowcircle_spr->height * gc_scale)) / 2;
                rdpq_sprite_blit(glowcircle_spr, gx, gy, &(rdpq_blitparms_t){ .scale_x = gc_scale, .scale_y = gc_scale });
            }
            float pulse = 0.75f + 0.25f * sinf(anim_t * 4.0f);
            font_begin(BLEND_ADD);
            rdpq_mode_filter(FILTER_BILINEAR);
            font_color(RGBA32(255 * pulse * 0.55f, 178 * pulse * 0.55f, 67 * pulse * 0.55f, 255));
            font_draw_scaled(FONT_GLOW, item_x, y + i * step, opt_labels[i], 1.10f);
        }
        font_begin(BLEND_ALPHA);
        rdpq_mode_filter(FILTER_BILINEAR);
        font_color(i == 2 && !settings.cheats ? RGBA32(196, 181, 80, 90) : armed ? C_MENU_ARMED : C_MENU);
        font_draw_scaled(FONT_MEDIUM, item_x, y + i * step, opt_labels[i], 1.10f);
    }
    if (screen == SCR_OPTIONS) {
        int hy = SH - 30;
        int text_x = x;
        if (buttons_a_spr) {
            float btn_scale = 0.459f;
            int fh = font_height(FONT_SMALL);
            int by = hy + (fh - (int)(buttons_a_spr->height * btn_scale)) / 2 - 2;
            rdpq_set_mode_standard();
            rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
            rdpq_mode_filter(FILTER_BILINEAR);
            rdpq_sprite_blit(buttons_a_spr, x, by, &(rdpq_blitparms_t){ .scale_x = btn_scale, .scale_y = btn_scale });
            text_x = x + 18;
        }
        // Drop shadow for hint text (85% opacity, bottom-right offset)
        font_begin(BLEND_ALPHA);
        font_color(RGBA32(0, 0, 0, 217));
        font_draw(FONT_SMALL, text_x + 1, hy + 1, opt_hints[opt_menu_sel]);
        // Main hint text in C_MENU
        font_begin(BLEND_ALPHA);
        font_color(C_MENU);
        font_draw(FONT_SMALL, text_x, hy, opt_hints[opt_menu_sel]);
    }
}

// a text button in the controller screens' style: amber outline and glow when armed
static void draw_menu_button(int x, int y, const char *label, bool armed)
{
    int w = (int)(font_width(FONT_MEDIUM, label) * 1.05f);
    int bx = x - 7, bw = w + 14, by = y - 2, bh = 18;
    if (armed) {
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_set_prim_color(RGBA32(255, 178, 67, 255));
        rdpq_fill_rectangle(bx + 1, by, bx + bw - 1, by + 1);
        rdpq_fill_rectangle(bx + 1, by + bh - 1, bx + bw - 1, by + bh);
        rdpq_fill_rectangle(bx, by + 1, bx + 1, by + bh - 1);
        rdpq_fill_rectangle(bx + bw - 1, by + 1, bx + bw, by + bh - 1);
        font_begin(BLEND_ALPHA);
        rdpq_mode_filter(FILTER_BILINEAR);
        font_color(RGBA32(0, 0, 0, 217));
        font_draw_scaled(FONT_MEDIUM, x + 1, y + 1, label, 1.05f);
        float pulse = 0.75f + 0.25f * sinf(anim_t * 4.0f);
        font_begin(BLEND_ADD);
        font_color(RGBA32(255 * pulse * 0.55f, 178 * pulse * 0.55f, 67 * pulse * 0.55f, 255));
        font_draw_scaled(FONT_GLOW, x, y, label, 1.05f);
        font_begin(BLEND_ALPHA);
        font_color(C_MENU_ARMED);
        font_draw_scaled(FONT_MEDIUM, x, y, label, 1.05f);
    } else {
        font_begin(BLEND_ALPHA);
        rdpq_mode_filter(FILTER_BILINEAR);
        font_color(RGBA32(0, 0, 0, 217));
        font_draw_scaled(FONT_MEDIUM, x + 1, y + 1, label, 1.05f);
        font_color(RGBA32(235, 235, 235, 255));
        font_draw_scaled(FONT_MEDIUM, x, y, label, 1.05f);
    }
}

// ---- shared pieces of the new menu's settings screens (Audio / Video style)
static void ui_load_common(void)
{
    if (!buttons_a_tried) { buttons_a_tried = true; FILE *f = fopen("rom:/ui/buttons_a.sprite", "rb"); if (f) { fclose(f); buttons_a_spr = sprite_load("rom:/ui/buttons_a.sprite"); } }
    if (!buttons_b_tried) { buttons_b_tried = true; FILE *f = fopen("rom:/ui/buttons_b.sprite", "rb"); if (f) { fclose(f); buttons_b_spr = sprite_load("rom:/ui/buttons_b.sprite"); } }
    if (!box_tried) { box_tried = true; FILE *f = fopen("rom:/ui/box.sprite", "rb"); if (f) { fclose(f); box_spr = sprite_load("rom:/ui/box.sprite"); } }
}
static void ui_header(int x, int y, const char *label)
{
    font_begin(BLEND_ALPHA);
    rdpq_mode_filter(FILTER_BILINEAR);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, x + 1, y + 1, label, 1.15f);
    font_color(RGBA32(235, 195, 65, 255));
    font_draw_scaled(FONT_MEDIUM, x, y, label, 1.15f);
}
// a label where '|' marks an extra pixel of space (letters the font runs together: "cl")
static void draw_spaced(int f, int x, int y, const char *label, float sc)
{
    char part[64];
    const char *p = label;
    while (*p) {
        int n = 0;
        while (p[n] && p[n] != '|' && n < 63) n++;
        memcpy(part, p, n); part[n] = 0;
        x = font_draw_scaled(f, x, y, part, sc);
        p += n;
        if (*p == '|') { p++; x += 2; }
    }
}
static void ui_item(int x, int y, const char *label, bool armed, color_t col)
{
    font_begin(BLEND_ALPHA);
    rdpq_mode_filter(FILTER_BILINEAR);
    font_color(RGBA32(0, 0, 0, 217));
    draw_spaced(FONT_MEDIUM, x + 1, y + 1, label, 1.05f);
    if (armed) {
        float pulse = 0.75f + 0.25f * sinf(anim_t * 4.0f);
        font_begin(BLEND_ADD);
        font_color(RGBA32(255 * pulse * 0.55f, 178 * pulse * 0.55f, 67 * pulse * 0.55f, 255));
        draw_spaced(FONT_GLOW, x, y, label, 1.05f);
    }
    font_begin(BLEND_ALPHA);
    font_color(armed ? C_MENU_ARMED : col);
    draw_spaced(FONT_MEDIUM, x, y, label, 1.05f);
}
static void ui_tickbox(int x, int y, bool on)
{
    if (!box_spr) return;
    rdpq_set_mode_standard();
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_filter(FILTER_BILINEAR);
    rdpq_sprite_blit(box_spr, x, y, &(rdpq_blitparms_t){ .s0 = 0, .t0 = 0, .width = 16, .height = 16 });
    if (on) rdpq_sprite_blit(box_spr, x, y, &(rdpq_blitparms_t){ .s0 = 16, .t0 = 0, .width = 16, .height = 16 });
}
// the bottom hint bar: (A) label, (B) label
static void ui_hints(const char *a_label, const char *b_label)
{
    int hy = SH - 22;
    float btn_scale = 0.459f;
    int bx = 170;
    if (a_label) {
        if (buttons_a_spr) {
            int by_btn = hy + (font_height(FONT_SMALL) - (int)(buttons_a_spr->height * btn_scale)) / 2 - 2;
            rdpq_set_mode_standard();
            rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
            rdpq_mode_filter(FILTER_BILINEAR);
            rdpq_sprite_blit(buttons_a_spr, 24, by_btn, &(rdpq_blitparms_t){ .scale_x = btn_scale, .scale_y = btn_scale });
        }
        font_begin(BLEND_ALPHA);
        font_color(RGBA32(0, 0, 0, 217));
        font_draw(FONT_SMALL, 42 + 1, hy + 1, a_label);
        font_color(C_MENU);
        font_draw(FONT_SMALL, 42, hy, a_label);
        int w = font_width(FONT_SMALL, a_label);
        if (42 + w + 24 > bx) bx = 42 + w + 24;
    }
    if (b_label) {
        if (!a_label) bx = 24;
        if (buttons_b_spr) {
            int by_btn = hy + (font_height(FONT_SMALL) - (int)(buttons_b_spr->height * btn_scale)) / 2 - 2;
            rdpq_set_mode_standard();
            rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
            rdpq_mode_filter(FILTER_BILINEAR);
            rdpq_sprite_blit(buttons_b_spr, bx, by_btn, &(rdpq_blitparms_t){ .scale_x = btn_scale, .scale_y = btn_scale });
        }
        font_begin(BLEND_ALPHA);
        font_color(RGBA32(0, 0, 0, 217));
        font_draw(FONT_SMALL, bx + 18 + 1, hy + 1, b_label);
        font_color(C_MENU);
        font_draw(FONT_SMALL, bx + 18, hy, b_label);
    }
}

// Options -> Controller Settings -> Button Mapping: each action and its button; A then a button
// to change one, the last row restores the standard layout
static void draw_button_mapping(void)
{
    draw_title_hl();
    ui_load_common();
    int hx = 28, x = 38, vx = 196, y = 80, step = 13;
    ui_header(hx, 58, "Button Mapping");
    for (int i = 0; i <= NUM_BINDS; i++) {
        int ry = y + i * step + (i == NUM_BINDS ? 4 : 0);
        bool armed = i == bind_sel;
        const char *l = i < NUM_BINDS ? bind_names[i] : "Restore standard layout";
        ui_item(x, ry, l, armed, C_MENU);
        if (i < NUM_BINDS) {
            const char *v = (armed && bind_listen) ? ((int)(anim_t * 3) & 1 ? "press a button" : "") : pad_names[settings.bind[i]];
            font_begin(BLEND_ALPHA);
            rdpq_mode_filter(FILTER_BILINEAR);
            font_color(RGBA32(0, 0, 0, 217));
            font_draw_scaled(FONT_MEDIUM, vx + 1, ry + 1, v, 1.05f);
            font_color(armed ? C_MENU_ARMED : RGBA32(235, 235, 235, 255));
            font_draw_scaled(FONT_MEDIUM, vx, ry, v, 1.05f);
        }
    }
    if (bind_listen) ui_hints("Press the new button  (Start: cancel)", NULL);
    else ui_hints(bind_sel < NUM_BINDS ? "Change" : "Restore", "Back");
}

// Options -> Cheat Codes
static int cheat_count(void) { return game_num_maps() > 0 ? 5 : 3; }
static void draw_cheats(void)
{
    draw_title_hl();
    ui_load_common();
    int hx = 28, x = 38, vx = 196;
    ui_header(hx, 60, "Cheats");
    ui_item(x, 84, "God mode", cheat_sel == 0, C_MENU);
    ui_tickbox(vx, 83, dbg_god);
    ui_item(x, 101, "No c|lipping", cheat_sel == 1, C_MENU);
    ui_tickbox(vx, 100, dbg_noclip);
    ui_item(x, 118, in_game ? "Give everything" : "Give everything (in game)", cheat_sel == 2, in_game ? C_MENU : RGBA32(196, 181, 80, 110));
    if (cheat_count() > 3) {
        ui_header(hx, 142, "Chapter select");
        ui_item(x, 166, "Map", cheat_sel == 3, C_MENU);
        char v[48];
        snprintf(v, sizeof(v), "%s", game_map_name(dbg_map));
        font_begin(BLEND_ALPHA);
        rdpq_mode_filter(FILTER_BILINEAR);
        font_color(RGBA32(0, 0, 0, 217));
        font_draw_scaled(FONT_MEDIUM, vx + 1, 167, v, 1.05f);
        font_color(cheat_sel == 3 ? C_MENU_ARMED : RGBA32(235, 235, 235, 255));
        font_draw_scaled(FONT_MEDIUM, vx, 166, v, 1.05f);
        ui_item(x, 183, "Go to map", cheat_sel == 4, C_MENU);
    }
    const char *a = cheat_sel <= 1 ? "Toggle" : cheat_sel == 2 ? "Give" : cheat_sel == 3 ? "Go  (Left / Right: pick map)" : "Go";
    ui_hints(a, "Back");
}

static void draw_controller_settings(void)
{
    draw_title_hl();

    if (!buttons_a_tried) {
        buttons_a_tried = true;
        FILE *f = fopen("rom:/ui/buttons_a.sprite", "rb");
        if (f) { fclose(f); buttons_a_spr = sprite_load("rom:/ui/buttons_a.sprite"); }
    }
    if (!buttons_b_tried) {
        buttons_b_tried = true;
        FILE *f = fopen("rom:/ui/buttons_b.sprite", "rb");
        if (f) { fclose(f); buttons_b_spr = sprite_load("rom:/ui/buttons_b.sprite"); }
    }
    if (!color2_tried) {
        color2_tried = true;
        FILE *f = fopen("rom:/ui/color2.sprite", "rb");
        if (f) { fclose(f); color2_spr = sprite_load("rom:/ui/color2.sprite"); }
    }
    if (!ctrl_tried) {
        ctrl_tried = true;
        FILE *f = fopen("rom:/ui/ctrl.sprite", "rb");
        if (f) { fclose(f); ctrl_spr = sprite_load("rom:/ui/ctrl.sprite"); }
    }

    // 1. Top row at cs_y = 54
    int cs_x = 14, cs_y = 54;
    bool cs_armed = (ctrl_sel == 0);

    // Label: Control Scheme: (Never lights up, always standard white/silver text)
    font_begin(BLEND_ALPHA);
    rdpq_mode_filter(FILTER_BILINEAR);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, cs_x + 1, cs_y + 1, "Control Scheme:", 1.05f);
    font_color(RGBA32(235, 235, 235, 255));
    font_draw_scaled(FONT_MEDIUM, cs_x, cs_y, "Control Scheme:", 1.05f);

    // Preset Box
    const char *cur_scheme = scheme_names[settings.control_scheme];
    int label_w = (int)(font_width(FONT_MEDIUM, "Control Scheme:") * 1.05f);
    int bx = cs_x + label_w + 14;
    int bw = (int)(font_width(FONT_MEDIUM, cur_scheme) * 1.05f) + 16;
    int by = cs_y - 2;
    int bh = 18;

    if (cs_armed) {
        // Outline box: only drawn when selected
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_set_prim_color(RGBA32(255, 178, 67, 255));
        rdpq_fill_rectangle(bx + 1, by, bx + bw - 1, by + 1);
        rdpq_fill_rectangle(bx + 1, by + bh - 1, bx + bw - 1, by + bh);
        rdpq_fill_rectangle(bx, by + 1, bx + 1, by + bh - 1);
        rdpq_fill_rectangle(bx + bw - 1, by + 1, bx + bw, by + bh - 1);

        // Drop shadow for preset name
        font_begin(BLEND_ALPHA);
        rdpq_mode_filter(FILTER_BILINEAR);
        font_color(RGBA32(0, 0, 0, 217));
        font_draw_scaled(FONT_MEDIUM, bx + 8 + 1, cs_y + 1, cur_scheme, 1.05f);

        // Glowing text when selected
        float pulse = 0.75f + 0.25f * sinf(anim_t * 4.0f);
        font_begin(BLEND_ADD);
        font_color(RGBA32(255 * pulse * 0.55f, 178 * pulse * 0.55f, 67 * pulse * 0.55f, 255));
        font_draw_scaled(FONT_GLOW, bx + 8, cs_y, cur_scheme, 1.05f);

        font_begin(BLEND_ALPHA);
        font_color(C_MENU_ARMED);
        font_draw_scaled(FONT_MEDIUM, bx + 8, cs_y, cur_scheme, 1.05f);

        // Arrow indicators: reflect position in preset array
        // Left arrow ◀ if not at start (index > 0)
        // Right arrow ▶ if not at end (index < 2)
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_set_prim_color(RGBA32(255, 178, 67, 255));

        int ay = cs_y + 2;
        if (settings.control_scheme > 0) {
            int lx = bx - 6;
            for (int r = 0; r < 5; r++) {
                rdpq_fill_rectangle(lx - r, ay + r, lx - r + 1, ay + 10 - r);
            }
        }
        if (settings.control_scheme < NUM_SCHEMES - 1) {
            int ax = bx + bw + 4;
            for (int r = 0; r < 5; r++) {
                rdpq_fill_rectangle(ax + r, ay + r, ax + r + 1, ay + 10 - r);
            }
        }
    } else {
        // Deselected: NO outline, NO arrows, text in standard white/silver
        font_begin(BLEND_ALPHA);
        rdpq_mode_filter(FILTER_BILINEAR);
        font_color(RGBA32(0, 0, 0, 217));
        font_draw_scaled(FONT_MEDIUM, bx + 8 + 1, cs_y + 1, cur_scheme, 1.05f);
        font_color(RGBA32(235, 235, 235, 255));
        font_draw_scaled(FONT_MEDIUM, bx + 8, cs_y, cur_scheme, 1.05f);
    }

    // 2. Controller Graphic: 3% smaller (188x145), aligned to the left
    int cw = 188, ch = 145;
    int cx = 12, cy = 72;
    int pad = 5;

    rdpq_set_mode_standard();
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_filter(FILTER_BILINEAR);
    if (color2_spr) {
        rdpq_sprite_blit(color2_spr, cx - pad, cy - pad, &(rdpq_blitparms_t){
            .scale_x = (float)(cw + pad * 2) / color2_spr->width,
            .scale_y = (float)(ch + pad * 2) / color2_spr->height,
        });
    }
    if (ctrl_spr) {
        rdpq_sprite_blit(ctrl_spr, cx, cy, NULL);
    }

    // 3. "Advanced..." and "Button Mapping..." buttons at the bottom-right
    draw_menu_button(230, 150, "Advanced...", ctrl_sel == 1);
    draw_menu_button(204, 174, "Button Mapping...", ctrl_sel == 2);

    // Bottom hint bar
    int hy = SH - 22;
    if (buttons_a_spr) {
        float btn_scale = 0.459f;
        int by_btn = hy + (font_height(FONT_SMALL) - (int)(buttons_a_spr->height * btn_scale)) / 2 - 2;
        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_sprite_blit(buttons_a_spr, 24, by_btn, &(rdpq_blitparms_t){ .scale_x = btn_scale, .scale_y = btn_scale });
    }
    font_begin(BLEND_ALPHA);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw(FONT_SMALL, 42 + 1, hy + 1, "Change / Select");
    font_color(C_MENU);
    font_draw(FONT_SMALL, 42, hy, "Change / Select");

    if (buttons_b_spr) {
        float btn_scale = 0.459f;
        int by_btn = hy + (font_height(FONT_SMALL) - (int)(buttons_b_spr->height * btn_scale)) / 2 - 2;
        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_sprite_blit(buttons_b_spr, 170, by_btn, &(rdpq_blitparms_t){ .scale_x = btn_scale, .scale_y = btn_scale });
    }
    font_begin(BLEND_ALPHA);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw(FONT_SMALL, 188 + 1, hy + 1, "Back");
    font_color(C_MENU);
    font_draw(FONT_SMALL, 188, hy, "Back");
}

static void draw_controller_advanced(void)
{
    draw_title_hl();

    if (!buttons_a_tried) {
        buttons_a_tried = true;
        FILE *f = fopen("rom:/ui/buttons_a.sprite", "rb");
        if (f) { fclose(f); buttons_a_spr = sprite_load("rom:/ui/buttons_a.sprite"); }
    }
    if (!buttons_b_tried) {
        buttons_b_tried = true;
        FILE *f = fopen("rom:/ui/buttons_b.sprite", "rb");
        if (f) { fclose(f); buttons_b_spr = sprite_load("rom:/ui/buttons_b.sprite"); }
    }
    if (!slider_tried) {
        slider_tried = true;
        FILE *f = fopen("rom:/ui/slider.sprite", "rb");
        if (f) { fclose(f); slider_spr = sprite_load("rom:/ui/slider.sprite"); }
    }
    if (!slider_end_cap_tried) {
        slider_end_cap_tried = true;
        FILE *f = fopen("rom:/ui/slider_end_cap.sprite", "rb");
        if (f) { fclose(f); slider_end_cap_spr = sprite_load("rom:/ui/slider_end_cap.sprite"); }
    }
    if (!slider_piece_tried) {
        slider_piece_tried = true;
        FILE *f = fopen("rom:/ui/slider_piece.sprite", "rb");
        if (f) { fclose(f); slider_piece_spr = sprite_load("rom:/ui/slider_piece.sprite"); }
    }
    if (!box_tried) {
        box_tried = true;
        FILE *f = fopen("rom:/ui/box.sprite", "rb");
        if (f) { fclose(f); box_spr = sprite_load("rom:/ui/box.sprite"); }
    }

    int lx = 44;

    // Header at y = 54
    font_begin(BLEND_ALPHA);
    rdpq_mode_filter(FILTER_BILINEAR);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, lx + 1, 54 + 1, "Advanced Controller Settings", 1.05f);
    font_color(RGBA32(235, 235, 235, 255));
    font_draw_scaled(FONT_MEDIUM, lx, 54, "Advanced Controller Settings", 1.05f);

    // Item 0: Look Sensitivity
    int sy = 84;
    bool s_armed = (adv_sel == 0);
    font_begin(BLEND_ALPHA);
    rdpq_mode_filter(FILTER_BILINEAR);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, lx + 1, sy + 1, "Look Sensitivity", 1.05f);
    if (s_armed) {
        float pulse = 0.75f + 0.25f * sinf(anim_t * 4.0f);
        font_begin(BLEND_ADD);
        font_color(RGBA32(255 * pulse * 0.55f, 178 * pulse * 0.55f, 67 * pulse * 0.55f, 255));
        font_draw_scaled(FONT_GLOW, lx, sy, "Look Sensitivity", 1.05f);
    }
    font_begin(BLEND_ALPHA);
    font_color(s_armed ? C_MENU_ARMED : C_MENU);
    font_draw_scaled(FONT_MEDIUM, lx, sy, "Look Sensitivity", 1.05f);

    int track_y = sy + 18;
    int num_pieces = 10;
    int track_w = 16 + num_pieces * 16 + 16;
    rdpq_set_mode_standard();
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_filter(FILTER_BILINEAR);
    if (slider_end_cap_spr && slider_piece_spr) {
        rdpq_sprite_blit(slider_end_cap_spr, lx, track_y, &(rdpq_blitparms_t){ .flip_x = true });
        for (int k = 0; k < num_pieces; k++) {
            rdpq_sprite_blit(slider_piece_spr, lx + 16 + k * 16, track_y, NULL);
        }
        rdpq_sprite_blit(slider_end_cap_spr, lx + 16 + num_pieces * 16, track_y, NULL);
    }
    if (slider_spr) {
        float frac = (float)(settings.look_sens - 1) / 9.0f;
        int travel = track_w - 20 - 8;
        int thumb_x = lx + 10 + (int)(frac * travel);
        int thumb_y = track_y + (16 - slider_spr->height) / 2;
        rdpq_sprite_blit(slider_spr, thumb_x, thumb_y, NULL);
    }
    char sens_str[8];
    snprintf(sens_str, sizeof(sens_str), "%d", settings.look_sens);
    font_begin(BLEND_ALPHA);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw(FONT_MEDIUM, lx + track_w + 12 + 1, track_y + 1, sens_str);
    font_color(s_armed ? C_MENU_ARMED : RGBA32(230, 230, 230, 255));
    font_draw(FONT_MEDIUM, lx + track_w + 12, track_y, sens_str);

    // Item 1: Reverse Up-Down Axis
    int iy_y = 132;
    bool iy_armed = (adv_sel == 1);
    if (box_spr) {
        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_sprite_blit(box_spr, lx, iy_y, &(rdpq_blitparms_t){ .s0 = 0, .t0 = 0, .width = 16, .height = 16 });
        if (settings.invert_y) {
            rdpq_sprite_blit(box_spr, lx, iy_y, &(rdpq_blitparms_t){ .s0 = 16, .t0 = 0, .width = 16, .height = 16 });
        }
    }
    font_begin(BLEND_ALPHA);
    rdpq_mode_filter(FILTER_BILINEAR);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, lx + 24 + 1, iy_y + 2 + 1, "Reverse Up-Down Axis", 1.05f);
    if (iy_armed) {
        float pulse = 0.75f + 0.25f * sinf(anim_t * 4.0f);
        font_begin(BLEND_ADD);
        font_color(RGBA32(255 * pulse * 0.55f, 178 * pulse * 0.55f, 67 * pulse * 0.55f, 255));
        font_draw_scaled(FONT_GLOW, lx + 24, iy_y + 2, "Reverse Up-Down Axis", 1.05f);
    }
    font_begin(BLEND_ALPHA);
    font_color(iy_armed ? C_MENU_ARMED : C_MENU);
    font_draw_scaled(FONT_MEDIUM, lx + 24, iy_y + 2, "Reverse Up-Down Axis", 1.05f);

    // Item 2: Enable Cheats
    int ch_y = 164;
    bool ch_armed = (adv_sel == 2);
    if (box_spr) {
        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_sprite_blit(box_spr, lx, ch_y, &(rdpq_blitparms_t){ .s0 = 0, .t0 = 0, .width = 16, .height = 16 });
        if (settings.cheats) {
            rdpq_sprite_blit(box_spr, lx, ch_y, &(rdpq_blitparms_t){ .s0 = 16, .t0 = 0, .width = 16, .height = 16 });
        }
    }
    font_begin(BLEND_ALPHA);
    rdpq_mode_filter(FILTER_BILINEAR);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, lx + 24 + 1, ch_y + 2 + 1, "Enable Cheats", 1.05f);
    if (ch_armed) {
        float pulse = 0.75f + 0.25f * sinf(anim_t * 4.0f);
        font_begin(BLEND_ADD);
        font_color(RGBA32(255 * pulse * 0.55f, 178 * pulse * 0.55f, 67 * pulse * 0.55f, 255));
        font_draw_scaled(FONT_GLOW, lx + 24, ch_y + 2, "Enable Cheats", 1.05f);
    }
    font_begin(BLEND_ALPHA);
    font_color(ch_armed ? C_MENU_ARMED : C_MENU);
    font_draw_scaled(FONT_MEDIUM, lx + 24, ch_y + 2, "Enable Cheats", 1.05f);

    // Bottom hint bar
    int hy = SH - 22;
    if (buttons_a_spr) {
        float btn_scale = 0.459f;
        int by_btn = hy + (font_height(FONT_SMALL) - (int)(buttons_a_spr->height * btn_scale)) / 2 - 2;
        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_sprite_blit(buttons_a_spr, 24, by_btn, &(rdpq_blitparms_t){ .scale_x = btn_scale, .scale_y = btn_scale });
    }
    font_begin(BLEND_ALPHA);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw(FONT_SMALL, 42 + 1, hy + 1, "Change / Toggle");
    font_color(C_MENU);
    font_draw(FONT_SMALL, 42, hy, "Change / Toggle");

    if (buttons_b_spr) {
        float btn_scale = 0.459f;
        int by_btn = hy + (font_height(FONT_SMALL) - (int)(buttons_b_spr->height * btn_scale)) / 2 - 2;
        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_sprite_blit(buttons_b_spr, 170, by_btn, &(rdpq_blitparms_t){ .scale_x = btn_scale, .scale_y = btn_scale });
    }
    font_begin(BLEND_ALPHA);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw(FONT_SMALL, 188 + 1, hy + 1, "Back");
    font_color(C_MENU);
    font_draw(FONT_SMALL, 188, hy, "Back");
}

static void draw_options_av(void)
{
    draw_title_hl();

    if (!buttons_a_tried) {
        buttons_a_tried = true;
        FILE *f = fopen("rom:/ui/buttons_a.sprite", "rb");
        if (f) { fclose(f); buttons_a_spr = sprite_load("rom:/ui/buttons_a.sprite"); }
    }
    if (!buttons_b_tried) {
        buttons_b_tried = true;
        FILE *f = fopen("rom:/ui/buttons_b.sprite", "rb");
        if (f) { fclose(f); buttons_b_spr = sprite_load("rom:/ui/buttons_b.sprite"); }
    }
    if (!slider_tried) {
        slider_tried = true;
        FILE *f = fopen("rom:/ui/slider.sprite", "rb");
        if (f) { fclose(f); slider_spr = sprite_load("rom:/ui/slider.sprite"); }
    }
    if (!slider_end_cap_tried) {
        slider_end_cap_tried = true;
        FILE *f = fopen("rom:/ui/slider_end_cap.sprite", "rb");
        if (f) { fclose(f); slider_end_cap_spr = sprite_load("rom:/ui/slider_end_cap.sprite"); }
    }
    if (!slider_piece_tried) {
        slider_piece_tried = true;
        FILE *f = fopen("rom:/ui/slider_piece.sprite", "rb");
        if (f) { fclose(f); slider_piece_spr = sprite_load("rom:/ui/slider_piece.sprite"); }
    }
    if (!box_tried) {
        box_tried = true;
        FILE *f = fopen("rom:/ui/box.sprite", "rb");
        if (f) { fclose(f); box_spr = sprite_load("rom:/ui/box.sprite"); }
    }

    int header_x = 28;
    int item_x = 38;
    int track_x = 126;

    // 1. Sound Section
    int sound_y = 60;
    font_begin(BLEND_ALPHA);
    rdpq_mode_filter(FILTER_BILINEAR);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, header_x + 1, sound_y + 1, "Sound", 1.15f);
    font_color(RGBA32(235, 195, 65, 255));
    font_draw_scaled(FONT_MEDIUM, header_x, sound_y, "Sound", 1.15f);

    // Item 0: Game volume
    int vol_y = 86;
    bool vol_armed = (av_sel == 0);
    font_begin(BLEND_ALPHA);
    rdpq_mode_filter(FILTER_BILINEAR);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, item_x + 1, vol_y + 1, "Game volume", 1.05f);
    if (vol_armed) {
        float pulse = 0.75f + 0.25f * sinf(anim_t * 4.0f);
        font_begin(BLEND_ADD);
        font_color(RGBA32(255 * pulse * 0.55f, 178 * pulse * 0.55f, 67 * pulse * 0.55f, 255));
        font_draw_scaled(FONT_GLOW, item_x, vol_y, "Game volume", 1.05f);
    }
    font_begin(BLEND_ALPHA);
    font_color(vol_armed ? C_MENU_ARMED : C_MENU);
    font_draw_scaled(FONT_MEDIUM, item_x, vol_y, "Game volume", 1.05f);

    // Game Volume Slider
    int num_pieces = 9;
    int track_w = 16 + num_pieces * 16 + 16;
    int track_y = vol_y - 2;
    rdpq_set_mode_standard();
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_filter(FILTER_BILINEAR);
    if (slider_end_cap_spr && slider_piece_spr) {
        rdpq_sprite_blit(slider_end_cap_spr, track_x, track_y, &(rdpq_blitparms_t){ .flip_x = true });
        for (int k = 0; k < num_pieces; k++) {
            rdpq_sprite_blit(slider_piece_spr, track_x + 16 + k * 16, track_y, NULL);
        }
        rdpq_sprite_blit(slider_end_cap_spr, track_x + 16 + num_pieces * 16, track_y, NULL);
    }
    if (slider_spr) {
        float frac = (float)settings.volume / 10.0f;
        int travel = track_w - 20 - 8;
        int thumb_x = track_x + 10 + (int)(frac * travel);
        int thumb_y = track_y + (16 - slider_spr->height) / 2;
        rdpq_sprite_blit(slider_spr, thumb_x, thumb_y, NULL);
    }

    // 2. Video Section
    int video_y = 126;
    font_begin(BLEND_ALPHA);
    rdpq_mode_filter(FILTER_BILINEAR);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, header_x + 1, video_y + 1, "Video", 1.15f);
    font_color(RGBA32(235, 195, 65, 255));
    font_draw_scaled(FONT_MEDIUM, header_x, video_y, "Video", 1.15f);

    // Item 1: Show frame rate
    int fps_y = 152;
    bool fps_armed = (av_sel == 1);
    font_begin(BLEND_ALPHA);
    rdpq_mode_filter(FILTER_BILINEAR);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, item_x + 1, fps_y + 1, "Show frame rate", 1.05f);
    if (fps_armed) {
        float pulse = 0.75f + 0.25f * sinf(anim_t * 4.0f);
        font_begin(BLEND_ADD);
        font_color(RGBA32(255 * pulse * 0.55f, 178 * pulse * 0.55f, 67 * pulse * 0.55f, 255));
        font_draw_scaled(FONT_GLOW, item_x, fps_y, "Show frame rate", 1.05f);
    }
    font_begin(BLEND_ALPHA);
    font_color(fps_armed ? C_MENU_ARMED : C_MENU);
    font_draw_scaled(FONT_MEDIUM, item_x, fps_y, "Show frame rate", 1.05f);

    // Tick box from advanced controller section with generous spacing
    int box_x = 180;
    int box_y = fps_y - 1;
    if (box_spr) {
        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_sprite_blit(box_spr, box_x, box_y, &(rdpq_blitparms_t){ .s0 = 0, .t0 = 0, .width = 16, .height = 16 });
        if (settings.show_stats) {
            rdpq_sprite_blit(box_spr, box_x, box_y, &(rdpq_blitparms_t){ .s0 = 16, .t0 = 0, .width = 16, .height = 16 });
        }
    }

    // Bottom hint bar
    int hy = SH - 22;
    if (buttons_a_spr) {
        float btn_scale = 0.459f;
        int by_btn = hy + (font_height(FONT_SMALL) - (int)(buttons_a_spr->height * btn_scale)) / 2 - 2;
        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_sprite_blit(buttons_a_spr, 24, by_btn, &(rdpq_blitparms_t){ .scale_x = btn_scale, .scale_y = btn_scale });
    }
    font_begin(BLEND_ALPHA);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw(FONT_SMALL, 42 + 1, hy + 1, "Change / Toggle");
    font_color(C_MENU);
    font_draw(FONT_SMALL, 42, hy, "Change / Toggle");

    if (buttons_b_spr) {
        float btn_scale = 0.459f;
        int by_btn = hy + (font_height(FONT_SMALL) - (int)(buttons_b_spr->height * btn_scale)) / 2 - 2;
        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_sprite_blit(buttons_b_spr, 170, by_btn, &(rdpq_blitparms_t){ .scale_x = btn_scale, .scale_y = btn_scale });
    }
    font_begin(BLEND_ALPHA);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw(FONT_SMALL, 188 + 1, hy + 1, "Back");
    font_color(C_MENU);
    font_draw(FONT_SMALL, 188, hy, "Back");
}

static void draw_credits(void)
{
    draw_title_hl();

    if (!buttons_b_tried) {
        buttons_b_tried = true;
        FILE *f = fopen("rom:/ui/buttons_b.sprite", "rb");
        if (f) { fclose(f); buttons_b_spr = sprite_load("rom:/ui/buttons_b.sprite"); }
    }
    if (!color2_tried) {
        color2_tried = true;
        FILE *f = fopen("rom:/ui/color2.sprite", "rb");
        if (f) { fclose(f); color2_spr = sprite_load("rom:/ui/color2.sprite"); }
    }

    // Centered translucent amber card holding the credits text
    int cw = 280, ch = 146;
    int cx = (SW - cw) / 2;
    int cy = 58;

    rdpq_set_mode_standard();
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_filter(FILTER_BILINEAR);
    if (color2_spr) {
        rdpq_sprite_blit(color2_spr, cx, cy, &(rdpq_blitparms_t){
            .scale_x = (float)cw / color2_spr->width,
            .scale_y = (float)ch / color2_spr->height,
        });
    }

    // Thin amber border outline around color2 card
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_set_prim_color(RGBA32(255, 178, 67, 180));
    rdpq_fill_rectangle(cx, cy, cx + cw, cy + 1);
    rdpq_fill_rectangle(cx, cy + ch - 1, cx + cw, cy + ch);
    rdpq_fill_rectangle(cx, cy, cx + 1, cy + ch);
    rdpq_fill_rectangle(cx + cw - 1, cy, cx + cw, cy + ch);

    // Credits text inside color2
    int tx = cx + 16;
    int y = cy + 12;

    // Block 1: Port to Nintendo 64: / ToxyCola
    font_begin(BLEND_ALPHA);
    rdpq_mode_filter(FILTER_BILINEAR);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, tx + 1, y + 1, "Port to Nintendo 64:", 1.05f);
    font_color(RGBA32(235, 195, 65, 255));
    font_draw_scaled(FONT_MEDIUM, tx, y, "Port to Nintendo 64:", 1.05f);

    y += 16;
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, tx + 1, y + 1, "ToxyCola", 1.05f);
    font_color(RGBA32(255, 255, 255, 255));
    font_draw_scaled(FONT_MEDIUM, tx, y, "ToxyCola", 1.05f);
    {
        int ux = tx + (int)(font_width(FONT_MEDIUM, "ToxyCola") * 1.05f) + 6;
        font_color(RGBA32(0, 0, 0, 217));
        font_draw_scaled(FONT_MEDIUM, ux + 1, y + 1, "using Claude Opus 5.5", 1.05f);
        font_color(RGBA32(196, 181, 80, 255));
        font_draw_scaled(FONT_MEDIUM, ux, y, "using Claude Opus 5.5", 1.05f);
    }

    // Block 2: Port of PS2 Menu \ Additional Graphics: / UnhealthyGrapejuice
    y += 20;
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, tx + 1, y + 1, "Port of PS2 Menu / Additional Graphics:", 1.05f);
    font_color(RGBA32(235, 195, 65, 255));
    font_draw_scaled(FONT_MEDIUM, tx, y, "Port of PS2 Menu / Additional Graphics:", 1.05f);

    y += 16;
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, tx + 1, y + 1, "UnhealthyGrapejuice", 1.05f);
    font_color(RGBA32(255, 255, 255, 255));
    font_draw_scaled(FONT_MEDIUM, tx, y, "UnhealthyGrapejuice", 1.05f);

    // Block 3: All assets, material and copyright are of Valve Corporation
    y += 24;
    font_color(RGBA32(0, 0, 0, 217));
    font_draw(FONT_SMALL, tx + 1, y + 1, "All assets, material and copyright are of Valve Corporation");
    font_color(RGBA32(215, 215, 215, 230));
    font_draw(FONT_SMALL, tx, y, "All assets, material and copyright are of Valve Corporation");

    // Block 4: This port utilises Libdragon & Tiny3D.
    y += 15;
    font_color(RGBA32(0, 0, 0, 217));
    font_draw(FONT_SMALL, tx + 1, y + 1, "This port utilises Libdragon & Tiny3D.");
    font_color(RGBA32(215, 215, 215, 230));
    font_draw(FONT_SMALL, tx, y, "This port utilises Libdragon & Tiny3D.");

    // Bottom hint bar
    int hy = SH - 22;
    if (buttons_b_spr) {
        float btn_scale = 0.459f;
        int by_btn = hy + (font_height(FONT_SMALL) - (int)(buttons_b_spr->height * btn_scale)) / 2 - 2;
        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_sprite_blit(buttons_b_spr, 24, by_btn, &(rdpq_blitparms_t){ .scale_x = btn_scale, .scale_y = btn_scale });
    }
    font_begin(BLEND_ALPHA);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw(FONT_SMALL, 42 + 1, hy + 1, "Back");
    font_color(C_MENU);
    font_draw(FONT_SMALL, 42, hy, "Back");
}

static void draw_options_legacy(void)
{
    int x = 24, y = 27, w = 272, h = 186;
    frame(x, y, w, h, "Options (Legacy)");
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
        static const char *help[NUM_SCHEMES][8] = {
            {"Stick: move", "C buttons: look", "Z: fire", "R: alt fire", "A: jump (hold: tuck)", "L: duck", "B: use", "D-pad: guns, reload, light"},
            {"Stick: look", "C buttons: move", "Z: fire", "R: alt fire", "A: jump (hold: tuck)", "L: duck", "B: use", "D-pad: guns, reload, light"},
            {"Stick: move / turn", "C buttons: look / strafe", "Z: fire", "R: alt fire", "A: jump (hold: tuck)", "L: duck", "B: use", "D-pad: guns, reload, light"},
            {"D-pad: move", "Stick: look", "Z: fire", "R: alt fire", "A: jump (hold: tuck)", "L: duck", "B: use", "C buttons: guns, reload, light"},
            {"D-pad: move", "C buttons: look", "Z: fire", "R: alt fire", "A: jump (hold: tuck)", "L: duck", "B: use", "Stick: guns, reload, light"},
        };
        int lh = font_height(FONT_SMALL), top = y + 40 + no * 16 + 2;
        for (int k = 0; k < 8; k++)
            text(FONT_SMALL, x + 20 + (k & 1) * 112, top + (k >> 1) * lh, C_TEXT_DIM, help[settings.control_scheme][k]);
    }
    button(x + w - 70, y + h - 22, 60, "Done (B)", true);
}

static void draw_leave(void)
{
    ui_load_common();
    int hx = 28;
    ui_header(hx, 92, "Leave game");
    font_begin(BLEND_ALPHA);
    rdpq_mode_filter(FILTER_BILINEAR);
    font_color(RGBA32(0, 0, 0, 217));
    font_draw_scaled(FONT_MEDIUM, 39, 117, "Return to the main menu? Unsaved progress is lost.", 1.0f);
    font_color(RGBA32(235, 235, 235, 255));
    font_draw_scaled(FONT_MEDIUM, 38, 116, "Return to the main menu? Unsaved progress is lost.", 1.0f);
    ui_item(48, 140, "Yes", leave_sel == 0, C_MENU);
    ui_item(110, 140, "No", leave_sel == 1, C_MENU);
    ui_hints("Select", "Back");
}

// the title backdrop is 150 KB: give it back before a map loads (maps need big contiguous blocks)
static sprite_t *title_bg;
static bool title_tried;
static sprite_t *menuglow_spr;
static bool menuglow_tried;

void menu_release_title(void)
{
    if (title_bg) { rspq_wait(); sprite_free(title_bg); title_bg = NULL; }
    if (glowcircle_spr) { rspq_wait(); sprite_free(glowcircle_spr); glowcircle_spr = NULL; }
    if (menuglow_spr) { rspq_wait(); sprite_free(menuglow_spr); menuglow_spr = NULL; }
    if (buttons_a_spr) { rspq_wait(); sprite_free(buttons_a_spr); buttons_a_spr = NULL; }
    if (buttons_b_spr) { rspq_wait(); sprite_free(buttons_b_spr); buttons_b_spr = NULL; }
    if (hl_top_spr) { rspq_wait(); sprite_free(hl_top_spr); hl_top_spr = NULL; }
    if (hl_bot_spr) { rspq_wait(); sprite_free(hl_bot_spr); hl_bot_spr = NULL; }
    if (color2_spr) { rspq_wait(); sprite_free(color2_spr); color2_spr = NULL; }
    if (ctrl_spr) { rspq_wait(); sprite_free(ctrl_spr); ctrl_spr = NULL; }
    if (slider_spr) { rspq_wait(); sprite_free(slider_spr); slider_spr = NULL; }
    if (slider_end_cap_spr) { rspq_wait(); sprite_free(slider_end_cap_spr); slider_end_cap_spr = NULL; }
    if (slider_piece_spr) { rspq_wait(); sprite_free(slider_piece_spr); slider_piece_spr = NULL; }
    if (box_spr) { rspq_wait(); sprite_free(box_spr); box_spr = NULL; }
    if (colorstrip2_spr) { rspq_wait(); sprite_free(colorstrip2_spr); colorstrip2_spr = NULL; }
    title_tried = false;
    colorstrip2_tried = false;
    glowcircle_tried = false;
    menuglow_tried = false;
    buttons_a_tried = false;
    buttons_b_tried = false;
    hl_top_tried = false;
    hl_bot_tried = false;
    color2_tried = false;
    ctrl_tried = false;
    slider_tried = false;
    slider_end_cap_tried = false;
    slider_piece_tried = false;
    box_tried = false;
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
        // Menu glow on the left, scrolling slowly down and looping back at the top
        if (!menuglow_tried) {
            menuglow_tried = true;
            menuglow_spr = sprite_load("rom:/ui/menuglow.sprite");
        }
        if (menuglow_spr) {
            rdpq_set_mode_standard();
            rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
            rdpq_mode_filter(FILTER_BILINEAR);
            float scroll_y = fmodf(menu_time * 18.0f, 128.0f);
            int sy = (int)scroll_y;
            int mg_x = 0;
            if (sy > 0) {
                rdpq_sprite_blit(menuglow_spr, mg_x, 0, &(rdpq_blitparms_t){ .t0 = 128 - sy, .height = sy });
            }
            rdpq_sprite_blit(menuglow_spr, mg_x, sy, NULL);
            int rem = SH - (sy + 128);
            if (rem > 0) {
                rdpq_sprite_blit(menuglow_spr, mg_x, sy + 128, &(rdpq_blitparms_t){ .t0 = 0, .height = rem });
            }
        }

    }
    if (screen == SCR_OPTIONS || screen == SCR_OPTIONS_LEGACY) draw_options();
    else if (screen == SCR_OPTIONS_CTRL) draw_controller_settings();
    else if (screen == SCR_OPTIONS_CTRL_ADV) draw_controller_advanced();
    else if (screen == SCR_OPTIONS_BINDS) draw_button_mapping();
    else if (screen == SCR_OPTIONS_AV) draw_options_av();
    else if (screen == SCR_OPTIONS_CREDITS) draw_credits();
    else if (screen == SCR_OPTIONS_CHEATS) draw_cheats();
    else if (screen == SCR_LOAD) draw_load();
    else if (screen == SCR_SAVE) draw_save();
    else if (screen == SCR_LEAVE) draw_title_hl();
    else draw_main();
    switch (screen) {
    case SCR_OPTIONS_LEGACY: draw_options_legacy(); break;
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