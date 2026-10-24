// HL64 - HUD text messages and screen fades, ported from the Half-Life client
// (cl_dll/message.cpp: CHudMessage) and the engine's titles.txt parser and screen fade.
#include <libdragon.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include "font.h"
#include "hud.h"

#define SCREEN_W 320
#define SCREEN_H 240
#define MAX_TITLES 320
#define MAX_HUD_MESSAGES 4
#define MSG_FONT FONT_SMALL

static textmsg_t titles[MAX_TITLES];
static int num_titles;
static char *titles_buf;

static const textmsg_t *messages[MAX_HUD_MESSAGES];
static float start_time[MAX_HUD_MESSAGES];
static textmsg_t custom_msgs[MAX_HUD_MESSAGES];
static char custom_text[MAX_HUD_MESSAGES][128];
static int custom_next;
static float hud_time;
static bool end_after_message, end_now;
static float game_title_time;
static int train_level = -1;

static image_t hud3;

// screen fade (engine screenfade_t)
static struct { float speed, end, reset; uint8_t r, g, b, alpha; int flags; bool active; } sf;

// ------------------------------------------------------------------ titles.txt (engine TextMessageParse)
static void parse_titles(void)
{
    int sz;
    char *d = asset_load("rom:/titles.txt", &sz);
    if (!d) return;
    titles_buf = d;
    textmsg_t cur = {.x = -1, .y = -1, .effect = 0, .r1 = 100, .g1 = 100, .b1 = 100, .r2 = 240, .g2 = 110, .b2 = 0,
                     .fadein = 0.01f, .fadeout = 1.5f, .holdtime = 3.5f, .fxtime = 0.25f};
    char *p = d, *end = d + sz;
    char *pending_name = NULL;
    while (p < end && num_titles < MAX_TITLES) {
        char *line = p;
        while (p < end && *p != '\n') p++;
        char *eol = p;
        if (p < end) p++;
        *eol = 0;
        if (eol > line && eol[-1] == '\r') eol[-1] = 0;
        char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (!*t || (t[0] == '/' && t[1] == '/')) continue;
        if (*t == '$') {
            char cmd[16];
            float a = 0, b = 0, c = 0;
            int n = sscanf(t + 1, "%15s %f %f %f", cmd, &a, &b, &c);
            if (n < 2) continue;
            if (!strcasecmp(cmd, "position")) { cur.x = a; cur.y = b; }
            else if (!strcasecmp(cmd, "effect")) cur.effect = (int)a;
            else if (!strcasecmp(cmd, "fxtime")) cur.fxtime = a;
            else if (!strcasecmp(cmd, "color")) { cur.r1 = a; cur.g1 = b; cur.b1 = c; }
            else if (!strcasecmp(cmd, "color2")) { cur.r2 = a; cur.g2 = b; cur.b2 = c; }
            else if (!strcasecmp(cmd, "fadein")) cur.fadein = a;
            else if (!strcasecmp(cmd, "fadeout")) cur.fadeout = a;
            else if (!strcasecmp(cmd, "holdtime")) cur.holdtime = a;
            continue;
        }
        if (*t == '{' && pending_name) {
            // message body: lines up to "}" (kept verbatim, joined with newlines)
            char *body = p;
            char *w = p;
            while (p < end) {
                char *l = p;
                while (p < end && *p != '\n') p++;
                char *le = p;
                if (p < end) p++;
                if (le > l && le[-1] == '\r') le--;
                char *s = l;
                while (s < le && (*s == ' ' || *s == '\t')) s++;
                if (s < le && *s == '}') break;
                if (w != body) *w++ = '\n';
                memmove(w, l, le - l);
                w += le - l;
            }
            *w = 0;
            textmsg_t *m = &titles[num_titles++];
            *m = cur;
            m->name = pending_name;
            m->text = body;
            pending_name = NULL;
            continue;
        }
        // a message name (the next line is "{")
        char *e = t + strlen(t);
        while (e > t && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        pending_name = t;
    }
    debugf("titles.txt: %d messages\n", num_titles);
}

static const textmsg_t *title_get(const char *name)
{
    for (int i = 0; i < num_titles; i++)
        if (!strcasecmp(titles[i].name, name)) return &titles[i];
    return NULL;
}

const char *hud_title_text(const char *name)
{
    const textmsg_t *m = title_get(name);
    return m ? m->text : NULL;
}

void hud_init(void)
{
    font_init();
    parse_titles();
    image_load(&hud3, "rom:/ui/hud3.sprite");
}

void hud_reset(void)
{
    memset(messages, 0, sizeof(messages));
    game_title_time = 0;
    train_level = -1;
    sf.active = false;
    end_after_message = end_now = false;
}

// ------------------------------------------------------------------ messages (CHudMessage)
static void message_add(const textmsg_t *m)
{
    for (int i = 0; i < MAX_HUD_MESSAGES; i++) {
        if (messages[i]) continue;
        for (int j = 0; j < MAX_HUD_MESSAGES; j++) {
            if (!messages[j]) continue;
            if (!strcmp(m->text, messages[j]->text)) return;     // already showing
            if (fabsf(m->y - messages[j]->y) < 0.0001f && fabsf(m->x - messages[j]->x) < 0.0001f)
                messages[j] = NULL;                              // one message per screen position
        }
        messages[i] = m;
        start_time[i] = hud_time;
        return;
    }
}

void hud_show_message(const char *name)
{
    if (!name || !name[0]) return;
    if (name[0] == '#') name++;
    if (!strcasecmp(name, "GAMETITLE")) { hud_show_game_title(); return; }
    if (!strcmp(name, "END3")) end_after_message = true;   // MsgFunc_HudText: leave the game afterwards
    const textmsg_t *m = title_get(name);
    if (!m) {
        // not in titles.txt: show the text itself (g_pCustomMessage)
        textmsg_t *c = &custom_msgs[custom_next];
        char *txt = custom_text[custom_next];
        custom_next = (custom_next + 1) % MAX_HUD_MESSAGES;
        *c = (textmsg_t){.x = -1, .y = 0.7f, .effect = 2, .r1 = 100, .g1 = 100, .b1 = 100, .r2 = 240, .g2 = 110,
                         .b2 = 0, .fadein = 0.01f, .fadeout = 1.5f, .fxtime = 0.25f, .holdtime = 5};
        strncpy(txt, name, 127); txt[127] = 0;
        c->text = txt;
        c->name = "custom";
        m = c;
    }
    message_add(m);
}

void hud_show_text(const textmsg_t *src)
{
    textmsg_t *c = &custom_msgs[custom_next];
    char *txt = custom_text[custom_next];
    custom_next = (custom_next + 1) % MAX_HUD_MESSAGES;
    *c = *src;
    strncpy(txt, src->text ? src->text : "", 127); txt[127] = 0;
    // game_text uses "\n" escapes
    for (char *s = txt; *s; s++) if (s[0] == '\\' && s[1] == 'n') { s[0] = '\n'; memmove(s + 1, s + 2, strlen(s + 2) + 1); }
    c->text = txt;
    c->name = "game_text";
    message_add(c);
}

void hud_show_game_title(void) { game_title_time = hud_time > 0 ? hud_time : 0.001f; }
void hud_set_train(int level) { train_level = level; }
bool hud_end_of_game(void) { bool e = end_now; end_now = false; return e; }

static float fade_blend(float fadein, float fadeout, float hold, float t)
{
    float fadetime = fadein + hold;
    if (t < 0) return 0;
    if (t < fadein) return 1 - ((fadein - t) / fadein);
    if (t > fadetime) return fadeout > 0 ? 1 - ((t - fadetime) / fadeout) : 0;
    return 1;
}

static int xpos(float x, int width, int total)
{
    int p;
    if (x == -1) p = (SCREEN_W - width) / 2;
    else if (x < 0) p = (1.0f + x) * SCREEN_W - total;
    else p = x * SCREEN_W;
    if (p + width > SCREEN_W) p = SCREEN_W - width;
    else if (p < 0) p = 0;
    return p;
}

static int ypos(float y, int height)
{
    int p;
    if (y == -1) p = (SCREEN_H - height) * 0.5f;
    else if (y < 0) p = (1.0f + y) * SCREEN_H - height;
    else p = y * SCREEN_H;
    if (p + height > SCREEN_H) p = SCREEN_H - height;
    else if (p < 0) p = 0;
    return p;
}

static int char_w(int c) { char s[2] = {(char)c, 0}; return font_width(MSG_FONT, s); }

// MessageDrawScan + MessageScanNextChar
static void draw_scan(const textmsg_t *m, float time)
{
    const char *s = m->text;
    int lines = 1, width = 0, total_w = 0, length = 0;
    for (const char *p = s; *p; p++, length++) {
        if (*p == '\n') { lines++; if (width > total_w) total_w = width; width = 0; }
        else width += char_w((unsigned char)*p);
    }
    if (width > total_w) total_w = width;
    int lh = font_height(MSG_FONT);
    int y = ypos(m->y, lines * lh);

    float fadetime, fade_blend_v = 0, char_time = 0;
    switch (m->effect) {
    case 2:
        fadetime = m->fadein * length + m->holdtime;
        fade_blend_v = (time > fadetime && m->fadeout > 0) ? ((time - fadetime) / m->fadeout) * 255 : 0;
        break;
    default:
        fadetime = m->fadein + m->holdtime;
        if (time < m->fadein) fade_blend_v = (m->fadein - time) * (1.0f / m->fadein) * 255;
        else if (time > fadetime) fade_blend_v = m->fadeout > 0 ? ((time - fadetime) / m->fadeout) * 255 : 255;
        else fade_blend_v = 0;
        if (m->effect == 1 && (rand() % 100) < 10) char_time = 1;
        break;
    }

    const char *p = s;
    for (int l = 0; l < lines; l++) {
        const char *ls = p;
        int lw = 0;
        while (*p && *p != '\n') lw += char_w((unsigned char)*p++);
        int x = xpos(m->x, lw, total_w);
        for (const char *c = ls; c < p; c++) {
            int sr = m->r1, sg = m->g1, sb = m->b1, dr = 0, dg = 0, db = 0, blend = 0;
            if (m->effect == 2) {
                char_time += m->fadein;
                if (char_time > time) { sr = sg = sb = 0; blend = 0; }
                else {
                    float dt = time - char_time;
                    if (time > fadetime) blend = fade_blend_v;
                    else if (dt > m->fxtime) blend = 0;
                    else { dr = m->r2; dg = m->g2; db = m->b2; blend = 255 - (dt * (1.0f / m->fxtime) * 255.0f + 0.5f); }
                }
            } else {
                blend = fade_blend_v;
            }
            if (blend > 255) blend = 255;
            if (blend < 0) blend = 0;
            int r = (sr * (255 - blend) + dr * blend) >> 8;
            int g = (sg * (255 - blend) + dg * blend) >> 8;
            int b = (sb * (255 - blend) + db * blend) >> 8;
            int cw = char_w((unsigned char)*c);
            if (m->effect == 1 && char_time != 0) {
                font_color(RGBA32(m->r2, m->g2, m->b2, 255));
                font_draw_char(MSG_FONT, x, y, (unsigned char)*c);
            }
            if (r | g | b) {
                font_color(RGBA32(r, g, b, 255));
                font_draw_char(MSG_FONT, x, y, (unsigned char)*c);
            }
            x += cw;
        }
        if (*p == '\n') p++;
        y += lh;
    }
}

void hud_update(float dt) { hud_time += dt; }

void hud_draw_messages(void)
{
    bool any = game_title_time > 0;
    for (int i = 0; i < MAX_HUD_MESSAGES; i++) any |= messages[i] != NULL;
    if (!any) return;
    font_begin(BLEND_ADD);
    if (game_title_time > 0) {
        const textmsg_t *gt = title_get("GAMETITLE");
        textmsg_t def = {.x = -1, .y = -1, .r1 = 180, .g1 = 180, .b1 = 180, .fadein = 1, .holdtime = 3, .fadeout = 1.5f};
        if (!gt) gt = &def;
        float t = hud_time - game_title_time;
        if (t > gt->fadein + gt->holdtime + gt->fadeout) game_title_time = 0;
        else if (hud3.spr) {
            float br = fade_blend(gt->fadein, gt->fadeout, gt->holdtime, t);
            // title_half (0,48,128,16) + title_life (128,48,116,16) from sprites/320hud3.spr
            int w = 128 + 116, h = 16;
            int x = xpos(gt->x, w, w), y = ypos(gt->y, h);
            font_color(RGBA32(br * gt->r1, br * gt->g1, br * gt->b1, 255));   // SPR_Set + SPR_DrawAdditive
            image_draw(&hud3, x, y, 0, 48, 128, 16);
            image_draw(&hud3, x + 128, y, 128, 48, 116, 16);
        }
    }
    for (int i = 0; i < MAX_HUD_MESSAGES; i++) {
        const textmsg_t *m = messages[i];
        if (!m) continue;
        float endt = start_time[i] + m->fadein + m->fadeout + m->holdtime;
        if (m->effect == 2) endt = start_time[i] + m->fadein * strlen(m->text) + m->fadeout + m->holdtime;
        if (hud_time <= endt) draw_scan(m, hud_time - start_time[i]);
        else {
            messages[i] = NULL;
            if (end_after_message) { end_after_message = false; end_now = true; }
        }
    }
}

// ------------------------------------------------------------------ screen fade (engine V_ScreenFade)
void hud_fade(const uint8_t c[4], float duration, float hold, int flags)
{
    sf.r = c[0]; sf.g = c[1]; sf.b = c[2]; sf.alpha = c[3];
    sf.flags = flags;
    sf.active = true;
    float end = duration, reset = hold;
    if (flags & FFADE_OUT) {
        sf.speed = end > 0 ? -(float)sf.alpha / end : 0;
        end += hud_time;
        reset += end;
    } else {
        sf.speed = end > 0 ? (float)sf.alpha / end : 0;
        reset += hud_time;
        end += reset;
    }
    sf.end = end;
    sf.reset = reset;
}

void hud_set_dark(void)
{
    // "startdark": black until the map's own env_fade fades in (safety: fade in by itself after 6s)
    uint8_t black[4] = {0, 0, 0, 255};
    hud_fade(black, 2.0f, 6.0f, 0);
}

void hud_draw_fade(void)
{
    if (!sf.active) return;
    if (hud_time > sf.reset && hud_time > sf.end && !(sf.flags & FFADE_STAYOUT)) { sf.active = false; return; }
    float a;
    if ((sf.flags & FFADE_STAYOUT) && hud_time > sf.end) a = sf.alpha;
    else {
        a = sf.speed * (sf.end - hud_time);
        if (sf.flags & FFADE_OUT) a += sf.alpha;
    }
    if (a > sf.alpha) a = sf.alpha;
    if (a <= 0) return;
    rdpq_set_mode_standard();
    // (modulate fades are drawn as plain blends)
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_set_prim_color(RGBA32(sf.r, sf.g, sf.b, (uint8_t)a));
    rdpq_fill_rectangle(0, 0, SCREEN_W, SCREEN_H);
}

// ------------------------------------------------------------------ train speed indicator (CHudTrain)
void hud_draw_train(void)
{
    if (train_level < 0 || !hud3.spr) return;
    font_begin(BLEND_ADD);
    font_color(RGBA32(255, 160, 0, 255));   // HUD colour (RGB_YELLOWISH)
    int lv = train_level > 4 ? 4 : train_level;
    // train_back/stop/forward1..3 in 320hud3.spr: 48x48 each along the top row
    image_draw(&hud3, SCREEN_W / 3 + 48 / 4, SCREEN_H - 48 - font_height(FONT_SMALL), lv * 48, 0, 48, 48);
}
