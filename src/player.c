// HL64 - the player: health, HEV suit, armour, damage, death, item pickups and the suit's voice
// (dlls/player.cpp, items.cpp, healthkit.cpp) and the health / armour / flashlight / pain HUD
// (cl_dll/health.cpp, battery.cpp, flashlight.cpp).
#include <libdragon.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "game.h"
#include "npc.h"
#include "font.h"
#include "hud.h"
#include "weapons.h"

player_t player;
static player_t snapshot;

void pm_get_view_ofs(float *v);
int  pm_get_groundent(void);

// suit voice (CBasePlayer::SetSuitUpdate / CheckSuitUpdate)
#define SUIT_QUEUE 4
#define SUIT_NOREPEAT 32
static char suit_queue[SUIT_QUEUE][24];
static bool suit_group[SUIT_QUEUE];
static float suit_next;
static struct { char name[24]; float until; } norepeat[SUIT_NOREPEAT];

static float max_fall;
static bool was_onground = true;
static float air_finished, drown_next, drown_dmg;
static float health_fade, armor_fade, flash_fade;
static float last_health = -1, last_armor = -1;
static float pain_alpha[4];   // front, right, back, left
static float dead_time;
static float flash_time;
static float lastdmg_time;

float player_max_health(void) { return 100; }
bool player_alive(void) { return player.health > 0 && !player.dead; }

void player_get_eye(float *eye)
{
    float o[3], v[3];
    pm_get_origin(o);
    pm_get_view_ofs(v);
    for (int k = 0; k < 3; k++) eye[k] = o[k] + v[k];
}

void player_new_game(void)
{
    memset(&player, 0, sizeof(player));
    player.health = 100;
    player.flashbattery = 100;
    memset(suit_queue, 0, sizeof(suit_queue));
    memset(norepeat, 0, sizeof(norepeat));
    weapons_reset();
}

void player_map_start(void)
{
    // the state the player arrived with: a death restarts the map from here (like the
    // autosave the PC game makes on every level change)
    if (player.dead || player.health <= 0) return;
    snapshot = player;
    memset(suit_queue, 0, sizeof(suit_queue));
    suit_next = 0;
    max_fall = 0;
    was_onground = true;
    air_finished = 0;
    for (int i = 0; i < SUIT_NOREPEAT; i++) norepeat[i].until = 0;
    memset(pain_alpha, 0, sizeof(pain_alpha));
}

void player_restore_map_start(void)
{
    player = snapshot;
    player.dead = false;
    if (player.health <= 0) player.health = 100;
}

bool player_take_health(float amount, int dmgtype)
{
    (void)dmgtype;
    if (!player_alive() || player.health >= 100) return false;
    player.health += amount;
    if (player.health > 100) player.health = 100;
    return true;
}

bool player_take_armor(float amount)
{
    if (!player.suit || player.armor >= 100) return false;
    player.armor += amount;
    if (player.armor > 100) player.armor = 100;
    return true;
}

void player_suit_update(const char *name, bool group, float nonrepeat)
{
    if (!player.suit || !name) return;
    float now = game_time();
    if (nonrepeat > 0) {
        int freeslot = -1;
        for (int i = 0; i < SUIT_NOREPEAT; i++) {
            if (norepeat[i].until > now && !strcasecmp(norepeat[i].name, name)) return;   // said recently
            if (norepeat[i].until <= now && freeslot < 0) freeslot = i;
        }
        if (freeslot >= 0) {
            strncpy(norepeat[freeslot].name, name, 23);
            norepeat[freeslot].until = now + nonrepeat;
        }
    }
    for (int i = 0; i < SUIT_QUEUE; i++) {
        if (!suit_queue[i][0]) {
            strncpy(suit_queue[i], name, 23);
            suit_group[i] = group;
            if (suit_next < now + 0.3f) suit_next = now + 0.3f;
            return;
        }
    }
}

// EMIT_SOUND_SUIT: the suit speaks on the player at "suitvolume"
static void suit_say(const char *s, bool group)
{
    int pitch = (rand() & 1) ? 98 + rand() % 7 : PITCH_NORM;
    if (group) snd_sentence(0, s, 0.6f, ATTN_NORM, pitch);
    else snd_emit(0, CHAN_STATIC, s, 0.6f, ATTN_NORM, pitch, NULL);
}

static void suit_think(void)
{
    float now = game_time();
    if (!player.suit || now < suit_next) return;
    for (int i = 0; i < SUIT_QUEUE; i++) {
        if (!suit_queue[i][0]) continue;
        suit_say(suit_queue[i], suit_group[i]);
        suit_queue[i][0] = 0;
        suit_next = now + 3.5f;   // SUITUPDATETIME
        return;
    }
}

void player_take_damage(float dmg, int type, const float *from, int attacker_ent)
{
    extern bool dbg_god;
    if (dbg_god) return;   // Debug menu: god mode
    (void)attacker_ent;
    if (!player_alive() || pm_noclip_active()) return;
    float prev = player.health;
    // armour (CBasePlayer::TakeDamage: ARMOR_RATIO 0.2, ARMOR_BONUS 0.5)
    if (player.armor > 0 && !(type & (DMG_FALL | DMG_DROWN))) {
        float flnew = dmg * 0.2f;
        float armor = (dmg - flnew) * 0.5f;
        if (armor > player.armor) { armor = player.armor * 2; flnew = dmg - armor; player.armor = 0; }
        else player.armor -= armor;
        dmg = flnew;
    }
    player.health -= (int)dmg;
    lastdmg_time = game_time();
    // view kick and the pain indicator for the direction it came from
    float p[3];
    pm_get_punch(p);
    p[0] -= dmg > 20 ? 4 : 2;
    pm_set_punch(p);
    if (from) {
        float o[3];
        pm_get_origin(o);
        float d[2] = {from[0] - o[0], from[1] - o[1]};
        float a = atan2f(d[1], d[0]) * 180 / (float)M_PI;
        extern float game_view_yaw(void);
        float rel = fmodf(a - game_view_yaw() + 720 + 45, 360);
        int dir = rel < 90 ? 0 : (rel < 180 ? 3 : (rel < 270 ? 2 : 1));
        pain_alpha[dir] = 1.0f;
    } else {
        for (int i = 0; i < 4; i++) pain_alpha[i] = fmaxf(pain_alpha[i], 0.6f);
    }
    if (player.health <= 0) {
        // CBasePlayer::Killed: blown apart by a big hit (health under -40, or an explosion)
        if (player.health < -40 || ((type & DMG_BLAST) && player.health < -10)) {
            extern void game_spawn_gibs(const float *o, int count, bool alien, float speed);
            float o[3];
            pm_get_origin(o);
            game_spawn_gibs(o, 10, false, 260);
        }
        player.health = 0;
        player.dead = true;
        dead_time = game_time();
        static const char *ds[] = {"player/pl_pain5.wav", "player/pl_pain6.wav", "player/pl_pain7.wav"};
        snd_emit(0, CHAN_VOICE, ds[rand() % 3], 1, ATTN_NORM, PITCH_NORM, NULL);
        if (player.suit) suit_say("HEV_DEAD", true);
        return;
    }
    // CBasePlayer::PainSound
    if (dmg >= 1 && !(type & (DMG_DROWN | DMG_FALL))) {
        int r = rand() % 6;
        if (r < 3) {
            static const char *ps[] = {"player/pl_pain5.wav", "player/pl_pain6.wav", "player/pl_pain7.wav"};
            snd_emit(0, CHAN_VOICE, ps[r], 1, ATTN_NORM, PITCH_NORM, NULL);
        }
    }
    // the suit's diagnosis
    bool trivial = player.health > 75 || dmg < 5, major = dmg > 25, critical = player.health < 30;
    if (!trivial) {
        if (type & DMG_CLUB && major) player_suit_update("!HEV_DMG4", false, 30);
        if (type & (DMG_FALL | DMG_CRUSH)) player_suit_update(major ? "!HEV_DMG5" : "!HEV_DMG4", false, 30);
        if ((type & DMG_BULLET) && dmg > 5) player_suit_update("!HEV_DMG6", false, 30);
        if (type & DMG_SLASH) player_suit_update(major ? "!HEV_DMG1" : "!HEV_DMG0", false, 30);
        if ((type & DMG_SONIC) && major) player_suit_update("!HEV_DMG2", false, 60);
        if (type & (DMG_POISON | DMG_PARALYZE)) player_suit_update("!HEV_DMG3", false, 60);
        if (type & DMG_ACID) player_suit_update("!HEV_DET1", false, 60);
        if (type & DMG_NERVEGAS) player_suit_update("!HEV_DET0", false, 60);
        if (type & DMG_RADIATION) player_suit_update("!HEV_DET2", false, 60);
    }
    if (!trivial && major && prev >= 75) {
        player_suit_update("!HEV_MED1", false, 1800);
        player_suit_update("!HEV_HEAL7", false, 1800);
    }
    if (!trivial && critical && prev < 75) {
        if (player.health < 6) player_suit_update("!HEV_HLTH3", false, 600);
        else if (player.health < 20) player_suit_update("!HEV_HLTH2", false, 600);
        if (rand() % 4 == 0 && prev < 50) player_suit_update("!HEV_DMG7", false, 300);
    }
}

// ------------------------------------------------------------------ items (items.cpp, healthkit.cpp, weapons)
bool player_touch_item(const char *cls, int npc_index)
{
    if (!player_alive()) return false;
    entity_t *e = &entities[npcs[npc_index].ent];
    if (!strcasecmp(cls, "item_suit")) {
        if (player.suit) return false;
        // CItemSuit::MyTouch: the suit logs on
        player.suit = true;
        player.weapons |= 1u << WEAPON_SUIT;
        suit_say((e->spawnflags & 1) ? "!HEV_A0" : "!HEV_AAx", false);
        suit_next = game_time() + 20;   // let the long logon play out
        return true;
    }
    if (!strcasecmp(cls, "item_battery")) {
        if (!player.suit || player.armor >= 100) return false;
        player.armor += skill_get("sk_battery");
        if (player.armor > 100) player.armor = 100;
        snd_emit(0, CHAN_ITEM, "items/gunpickup2.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        int pct = (int)(player.armor * 100.0f / 100.0f + 0.5f) / 5;
        if (pct > 0) pct--;
        char s[24];
        snprintf(s, sizeof(s), "!HEV_%dP", pct);
        player_suit_update(s, false, 30);
        return true;
    }
    if (!strcasecmp(cls, "item_healthkit")) {
        if (!player_take_health(skill_get("sk_healthkit"), DMG_GENERIC)) return false;
        snd_emit(0, CHAN_ITEM, "items/smallmedkit1.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        return true;
    }
    if (!strcasecmp(cls, "item_longjump")) {
        if (player.longjump || !player.suit) return false;
        player.longjump = true;
        suit_say("!HEV_A1", false);
        return true;
    }
    if (!strcasecmp(cls, "item_antidote") || !strcasecmp(cls, "item_security")) {
        snd_emit(0, CHAN_ITEM, "items/gunpickup2.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        if (!strcasecmp(cls, "item_antidote")) player_suit_update("!HEV_DET4", false, 60);
        return true;
    }
    if (!strncasecmp(cls, "weapon_", 7)) {
        const char *c = cls;
        if (!strcasecmp(c, "weapon_glock")) c = "weapon_9mmhandgun";
        else if (!strcasecmp(c, "weapon_python")) c = "weapon_357";
        else if (!strcasecmp(c, "weapon_mp5")) c = "weapon_9mmAR";
        return weapons_give(c);
    }
    if (!strncasecmp(cls, "ammo_", 5) || !strcasecmp(cls, "weaponbox")) return weapons_give_ammo(cls);
    return false;
}

// ------------------------------------------------------------------ per frame
void player_think(float dt)
{
    float now = game_time();
    if (player.dead) {
        // the view sinks to the floor (CBasePlayer::PlayerDeathThink / V_CalcRefdef)
        extern void pm_set_view_ofs_z(float z);
        float v[3];
        pm_get_view_ofs(v);
        if (v[2] > -28) pm_set_view_ofs_z(v[2] - dt * 60);
        return;
    }
    suit_think();
    // falling damage (CBasePlayer::PostThink: PLAYER_MAX_SAFE_FALL_SPEED 580, fatal 1024)
    bool onground = pm_get_groundent() >= 0;
    float fv = pm_get_fall_velocity();
    if (!onground && fv > max_fall) max_fall = fv;
    if (onground && !was_onground) {
        if (max_fall > 580 && pm_get_waterlevel() == 0 && !pm_noclip_active()) {
            float dmg = (max_fall - 580) * 100.0f / (1024 - 580);
            player_take_damage(dmg, DMG_FALL, NULL, -1);
        }
        max_fall = 0;
    }
    if (onground) max_fall = 0;
    was_onground = onground;
    // drowning (CBasePlayer::WaterMove: 12 seconds of air, then rising damage every second)
    if (pm_get_waterlevel() == 3 && !pm_noclip_active()) {
        if (air_finished == 0) air_finished = now + 12;
        if (now > air_finished && now > drown_next) {
            drown_dmg = drown_dmg < 5 ? drown_dmg + 1 : 5;
            drown_next = now + 1;
            player_take_damage(drown_dmg, DMG_DROWN, NULL, -1);
            snd_emit(0, CHAN_VOICE, rand() & 1 ? "player/pl_drown1.wav" : "player/pl_drown1.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        }
    } else {
        if (air_finished && now > air_finished) snd_emit(0, CHAN_VOICE, "player/pl_wade1.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
        air_finished = 0;
        drown_dmg = 0;
    }
    // flashlight battery (FLASH_DRAIN_TIME 1.2, FLASH_CHARGE_TIME 0.2)
    if (now > flash_time) {
        if (player.flashlight) {
            if (player.flashbattery > 0) player.flashbattery--;
            if (player.flashbattery <= 0) player.flashlight = false;
            flash_time = now + 1.2f;
        } else if (player.flashbattery < 100) {
            player.flashbattery++;
            flash_time = now + 0.2f;
        }
    }
    for (int i = 0; i < 4; i++) if (pain_alpha[i] > 0) pain_alpha[i] -= dt * 0.8f;
}

void player_toggle_flashlight(void)
{
    if (!player.suit || !player_alive()) return;
    if (!player.flashlight && player.flashbattery <= 0) return;
    player.flashlight = !player.flashlight;
    snd_emit(0, CHAN_WEAPON, "items/flashlight1.wav", 1, ATTN_NORM, PITCH_NORM, NULL);
}

float player_dead_time(void) { return player.dead ? game_time() - dead_time : 0; }

// ------------------------------------------------------------------ HUD
static image_t pain_img;
static bool pain_loaded;
void player_hud_init(void) { if (!pain_loaded) { pain_loaded = true; image_load(&pain_img, "rom:/ui/pain.sprite"); } }

static color_t col(float a, bool red)
{
    if (red) return RGBA32(250 * a / 255, 0, 0, 255);
    return RGBA32(255 * a / 255, 160 * a / 255, 0, 255);
}

void player_draw_hud(float dt)
{
    if (!pain_loaded) { pain_loaded = true; image_load(&pain_img, "rom:/ui/pain.sprite"); }
    // pain direction indicators (CHudHealth::DrawPain)
    font_begin(BLEND_ADD);
    static const int fx[4] = {0, 64, 88, 152}, fw[4] = {64, 24, 64, 24}, fh[4] = {24, 64, 24, 64};
    for (int i = 0; i < 4; i++) {
        if (pain_alpha[i] <= 0 || !player.suit) continue;
        font_color(RGBA32(255 * fminf(pain_alpha[i], 1), 0, 0, 255));
        int x = 160 - fw[i] / 2, y = 120 - fh[i] / 2;
        if (i == 0) y = 120 - fh[i] * 3;
        if (i == 2) y = 120 + fh[i] * 2;
        if (i == 1) x = 160 + fw[i] * 2;
        if (i == 3) x = 160 - fw[i] * 3;
        image_draw(&pain_img, x, y, fx[i], 0, fw[i], fh[i]);
    }
    if (!player.suit) return;   // no HUD without the HEV suit
    // fade the numbers from full brightness down to MIN_ALPHA when they change
    if (player.health != last_health) { health_fade = 255; last_health = player.health; }
    if (player.armor != last_armor) { armor_fade = 255; last_armor = player.armor; }
    health_fade = fmaxf(100, health_fade - dt * 200);
    armor_fade = fmaxf(100, armor_fade - dt * 200);
    int y = 240 - 16 - 8 - 8;
    bool red = player.health <= 25;
    // health: cross + number + divider
    font_color(col(health_fade, red));
    hud_icon2(8 + 4, y, 0, 72, 16, 16);
    int x = hud_number(16 + 6 + 4, y, (int)player.health, 3, true);
    rdpq_set_mode_fill(col(health_fade, red));
    rdpq_fill_rectangle(x + 6, y, x + 7, y + 16);
    // armour: the suit icon fills up with the charge (CHudBattery::Draw)
    font_begin(BLEND_ADD);
    font_color(col(armor_fade, false));
    int ax = 320 / 5 + 8;
    hud_icon2(ax, y - 3, 0, 52, 20, 20);
    int full = (int)(20 * player.armor / 100);
    if (full > 0) hud_icon2(ax, y - 3 + (20 - full), 20, 52 + (20 - full), 20, full);
    hud_number(ax + 20, y, (int)player.armor, 3, true);
    // flashlight (CHudFlashlight)
    font_color(col(player.flashlight ? 255 : 120, false));
    hud_icon2(320 - 18 - 12, 10, player.flashlight ? 16 : 34, 72, 18, 16);
    if (player.flashlight) hud_icon2(320 - 18 - 12 - 6, 10, 52, 72, 6, 16);
    rdpq_set_mode_standard();
}
