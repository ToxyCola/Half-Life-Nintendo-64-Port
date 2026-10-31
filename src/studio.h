// HL64 - Half-Life studio models (characters / monsters) on the N64
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <t3d/t3d.h>

typedef struct { char name[32]; float fps; uint32_t flags; uint16_t numframes, activity; float linmove[3];
                 uint32_t animofs; uint16_t numevents, pad; uint32_t eventofs; } sseq_t;
typedef struct { uint16_t tw, th, w, h; uint32_t dofs, pofs, flags; } stex_t;
typedef struct { uint16_t nummodels, base; uint32_t firstmodel; } sbodypart_t;
typedef struct { uint32_t firstchunk, numchunks; } ssubmodel_t;
typedef struct { uint32_t firstload; uint16_t numloads, numverts; uint32_t firsttri; uint16_t numtris, skinref; } schunk_t;
typedef struct { uint8_t bone, cofs, cnt, pad; uint32_t vofs; } sload_t;
typedef struct { int16_t q[4]; int16_t p[3]; int16_t pad; } sbonekey_t;
typedef struct { uint16_t frame, event; char options[60]; } sevent_t;
typedef struct { int16_t bone, pad; float org[3]; } satt_t;

typedef struct studio_s {
    char name[32];
    void *base;
    float posscale;
    int numskinref, numskinfamilies;
    float eye[3], hullmin[3], hullmax[3];
    int mouth_bone, mouth_type; float mouth_start, mouth_end;   // bone controller 4
    int ctl0_bone, ctl0_type; float ctl0_start, ctl0_end;       // bone controller 0 (translation)
    int16_t *parents;      int numbones;
    sseq_t *seqs;          int numseqs;
    uint8_t *anim;
    stex_t *tex;           int numtex;
    uint8_t *texdata;
    int16_t *skins;
    sbodypart_t *bodyparts; int numbodyparts;
    ssubmodel_t *submodels;
    schunk_t *chunks;      int numchunks;
    sload_t *loads;
    T3DVertPacked *verts;
    uint8_t *tris;
    sevent_t *events;
    satt_t *att;           int numatt;
    void **texblocks;      // rspq_block_t* per texture
    int16_t *strip_idx;    // tiny3d strip index lists (DMEM format), per chunk: strips [cs_first, +cs_count)
    uint32_t *strip_ofs;   // per strip: offset (int16 units) << 8 | count
    uint16_t *cs_first, *cs_count;
} studio_t;

studio_t *studio_get(const char *name);      // load (cached per map) "scientist" -> rom:/models/scientist.hlm
void studio_free_all(void);
void studio_free(studio_t *m);
void studio_set_filter(const char **names, int count, const int *acts, int nacts);
int  studio_find_seq(studio_t *m, const char *name);
int  studio_find_activity(studio_t *m, int activity);
float studio_seq_duration(studio_t *m, int seq);

typedef struct {
    studio_t *model;
    float origin[3];
    float yaw;
    int seq;
    float frame;
    int body, skin;
    uint8_t light[3];
    float mouth;             // 0..1 jaw open (talking)
    float ctl0;              // bone controller 0 value (units), e.g. the barnacle's tongue
    bool use_xform;          // xform instead of origin + yaw (the first-person view model)
    float xform[3][4];
    float (*att_out)[3];     // optional: world positions of the first att_n attachments
    int att_n;
    int16_t *cache_slot;     // optional: where a fixed pose's cached bone matrices are (-1 none)
    bool cacheable;          // the pose is final (a body on the floor): cache it
    const void *lod_inst;    // optional: the instance (npc) this is, for half-rate animation far away
    bool lod_half;           // far away: reuse last frame's bones every other frame
} studio_draw_t;

void studio_begin_frame(int frame_index);
void studio_draw(const studio_draw_t *d);
