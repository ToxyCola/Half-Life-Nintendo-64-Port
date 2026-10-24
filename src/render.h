#pragma once
#include <libdragon.h>

typedef struct { int tris, faces, ents, leaf, npcs; uint32_t t_wait, t_vis, t_world, t_ents, t_sky; } render_stats_t;
extern render_stats_t rstats;
extern color_t sky_color;

void render_init(void);
void render_new_map(void);
void render_frame(const float *eye, const float *angles, void (*hud)(void));
void render_load_sky(const char *name);
void render_free_map(void);
