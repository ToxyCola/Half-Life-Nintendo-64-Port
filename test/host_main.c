// PC test harness: runs the N64 game logic (Valve pm_shared + HL64 collision/entities) on a converted map.
#include <libdragon.h>
#undef fopen
#include <math.h>
#include "bsp.h"
#include "game.h"
static const char *rootdir = ".";
FILE *host_fopen(const char *path, const char *mode)
{
    char buf[512];
    if (!strncmp(path, "rom:/", 5)) { snprintf(buf, sizeof(buf), "%s/%s", rootdir, path + 5); path = buf; }
    return fopen(path, mode);
}
void snd_play(const char *s, float v, int c) { if (s && getenv("SNDLOG")) fprintf(stderr, "snd %s\n", s); }
void game_add_view_yaw(float d) {}
void snd_stop_all(void) {}
void hud_message(const char *m) { fprintf(stderr, "HUD: %s\n", m); }
// HUD / camera / NPC stand-ins (the harness has no renderer or characters)
#include "hud.h"
#include "npc.h"
void hud_fade(const uint8_t c[4], float d, float h, int f) { fprintf(stderr, "FADE %d %d %d %d %.1f %.1f %d\n", c[0], c[1], c[2], c[3], d, h, f); }
void hud_show_message(const char *n) { fprintf(stderr, "MESSAGE %s\n", n); }
void hud_show_text(const textmsg_t *m) { fprintf(stderr, "TEXT %s\n", m->text); }
void game_set_camera(int e, float w, bool f) { fprintf(stderr, "CAMERA %d %.1f %d\n", e, w, f); }
void game_end_section(void) { fprintf(stderr, "ENDSECTION\n"); }
void game_set_view_angles(const float *a) { fprintf(stderr, "VIEWANGLES %.0f %.0f\n", a[0], a[1]); }
void npc_script_use(int sci) { ents_fire_entity_target(&entities[sci]); }
int npcs_get_boxes(npc_box_t *o, int m, const float *n, float r) { return 0; }
bool npc_origin_for_entity(int ent, float *o) { return false; }
void snd_set_volume(float v) {}
static char pend_map[32], pend_lm[48]; static int pend;
void game_request_changelevel(const char *map, const char *lm) { fprintf(stderr, "CHANGELEVEL %s %s\n", map, lm ? lm : ""); strcpy(pend_map, map); strcpy(pend_lm, lm ? lm : ""); pend = 1; }
void pm_set_origin(const float *o);
static void do_change(void)
{
    pend = 0;
    float old[3] = {0}, p[3];
    int i = ents_find_class("info_landmark", 0);
    for (; i >= 0; i = ents_find_class("info_landmark", i + 1)) if (!strcasecmp(entities[i].targetname, pend_lm)) break;
    if (i < 0) return;
    memcpy(old, entities[i].origin, 12);
    ents_save_globals(old);
    pm_get_origin(p);
    char path[64]; snprintf(path, sizeof(path), "%s/maps/%s.hlb", rootdir, pend_map);
    FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "map %s not available\n", pend_map); return; } fclose(f);
    bsp_load(pend_map); ents_spawn_all();
    for (i = ents_find_class("info_landmark", 0); i >= 0; i = ents_find_class("info_landmark", i + 1))
        if (!strcasecmp(entities[i].targetname, pend_lm)) {
            float n[3]; for (int k = 0; k < 3; k++) n[k] = p[k] - old[k] + entities[i].origin[k];
            pm_find_free_position(n); pm_set_origin(n); ents_restore_globals(entities[i].origin); break;
        }
}
void pm_get_origin(float *o); void pm_reset_player(const float *o); void pm_clear_basevelocity(void);
void pm_get_velocity(float *v);
int pm_get_groundent(void);

int main(int argc, char **argv)
{
    rootdir = argv[1];
    if (!bsp_load(argv[2])) return 1;
    ents_spawn_all();
    pm_init();
    int s = ents_find_class("info_player_start", 0);
    float o[3]; memcpy(o, entities[s].origin, 12); o[2] += 1;
    pm_find_free_position(o);
    if (getenv("START")) sscanf(getenv("START"), "%f %f %f", &o[0], &o[1], &o[2]);
    pm_reset_player(o);
    if (getenv("CHANGE")) { char m[32], l[48]; sscanf(getenv("CHANGE"), "%31s %47s", m, l); game_request_changelevel(m, l); }
    float yaw = entities[s].angles[1], dt = 1 / 30.0f;
    // script: lines of "t_start t_end fwd side yawrate buttons"
    FILE *sc = fopen(argv[3], "r");
    float ev[64][6]; int nev = 0;
    while (nev < 64 && fscanf(sc, "%f %f %f %f %f %f", &ev[nev][0], &ev[nev][1], &ev[nev][2], &ev[nev][3], &ev[nev][4], &ev[nev][5]) == 6) nev++;
    float T = atof(argv[4]);
    for (float t = 0; t < T; t += dt) {
        player_input_t in = {0};
        for (int i = 0; i < nev; i++) if (t >= ev[i][0] && t < ev[i][1]) {
            in.forward += ev[i][2]; in.side += ev[i][3]; yaw += ev[i][4] * dt; in.buttons |= (int)ev[i][5];
        }
        in.viewangles[1] = yaw;
        pm_clear_basevelocity();
        ents_think(dt);
        pm_run(&in, dt);
        if (pend) do_change();
        pm_unstick(dt);
        float p[3], v[3]; pm_get_origin(p); pm_get_velocity(v);
        if (getenv("VERBOSE") || fmodf(t, 0.5f) < dt)
            printf("t=%.2f pos %.1f %.1f %.1f vel %.0f %.0f %.0f yaw %.0f ground %d hull1 %d\n", t, p[0], p[1], p[2], v[0], v[1], v[2], yaw,
                   pm_get_groundent(), bsp_hull_point_contents(&world.models[0].hulls[1], world.models[0].hulls[1].firstclipnode, p));
    }
    return 0;
}
