// HL64 - Half-Life map runtime for Nintendo 64
// Loads .hlb files produced by tools/bsp2n64.py (big-endian, N64-native layout).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <t3d/t3d.h>

// GoldSrc contents values (common/const.h)
#ifndef CONTENTS_EMPTY
#define CONTENTS_EMPTY  -1
#define CONTENTS_SOLID  -2
#define CONTENTS_WATER  -3
#define CONTENTS_SLIME  -4
#define CONTENTS_LAVA   -5
#define CONTENTS_SKY    -6
#endif

#define FF_SKY       1
#define FF_NODRAW    2
#define FF_ALPHATEST 4
#define FF_WATER     8

typedef struct { float normal[3]; float dist; uint8_t type, signbits, pad[2]; } bplane_t;      // == mplane_t
typedef struct { int32_t planenum; int32_t children[2]; } bclipnode_t;  // like dclipnode_t, 32-bit children
typedef struct { int32_t planenum; int16_t children[2]; int16_t mins[3], maxs[3]; uint16_t firstface, numfaces; } bnode_t;
typedef struct { int32_t contents; int32_t visofs; int16_t mins[3], maxs[3]; uint16_t firstmark, nummark; uint8_t ambient[4]; } bleaf_t;
typedef struct { float mins[3], maxs[3], origin[3]; int32_t headnode[4]; int32_t visleafs, firstface, numfaces; } bmodel_t;
typedef struct { uint16_t planenum; uint8_t side, flags; uint16_t texture, pad; uint32_t firstchunk; uint16_t numchunks, pad2; } bface_t;
typedef struct { float s[4], t[4]; int32_t texmins[2], extents[2]; int32_t miptex; } bfacetex_t;
typedef struct { uint32_t firstvert; uint8_t numverts, numtris; uint16_t pad; uint32_t firsttri; } bchunk_t;
typedef struct { char name[16]; uint16_t w, h, origw, origh, flags, pad; uint32_t dataofs, palofs; } btexture_t;

// Same layout as the hull_t defined privately inside Valve's pm_shared.c
typedef struct hull_s {
    bclipnode_t *clipnodes;
    bplane_t    *planes;
    int          firstclipnode, lastclipnode;
    float        clip_mins[3], clip_maxs[3];
} hull_t;

// Opaque to pm_shared.c: one of these per inline BSP model (*0 = world, *1.. = brush entities)
typedef struct model_s {
    bmodel_t *bm;
    int       index;
    hull_t    hulls[4];
} model_t;

typedef struct {
    void        *base;
    int          vertscale;
    bplane_t    *planes;    int numplanes;
    bclipnode_t *clipnodes; int numclipnodes;
    bnode_t     *nodes;     int numnodes;
    bleaf_t     *leafs;     int numleafs;
    uint16_t    *marks;     int nummarks;
    uint8_t     *vis;       int visbytes;
    bmodel_t    *bmodels;   int nummodels;
    bface_t     *faces;     int numfaces;
    bfacetex_t  *facetex;
    bchunk_t    *chunks;    int numchunks;
    T3DVertPacked *verts;   int numverts;
    uint8_t *lightstyles;    int numlightstyles;   // relit vertices (lights.c)
    uint8_t     *tris;      int numtris;
    btexture_t  *textures;  int numtextures;
    uint8_t     *texdata;
    uint8_t     *texblock;  // textures loaded from the shared pack (v2 files)
    char        *entities;
    model_t     *models;
    char         name[32];
} world_t;

extern world_t world;

bool  bsp_load(const char *mapname);
void  bsp_free(void);
int   bsp_leaf_for_point(const float *p);
const uint8_t *bsp_leaf_pvs(int leaf);  // decompressed, valid until next call
int   bsp_hull_point_contents(hull_t *hull, int num, const float *p);
int   bsp_point_contents(const float *p);   // world hull 0

typedef struct {
    bool  allsolid, startsolid, inopen, inwater;
    float fraction;
    float endpos[3];
    float normal[3];
    float dist;
} btrace_t;
void  bsp_trace_hull(hull_t *hull, const float *start, const float *end, btrace_t *tr);
const char *bsp_trace_texture(int modelindex, const float *offset, const float *start, const float *end);
void  bsp_light_at(const float *p, uint8_t *rgb);
