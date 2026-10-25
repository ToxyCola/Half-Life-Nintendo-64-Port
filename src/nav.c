// HL64 - monster navigation: the map's node graph (built offline by tools/navgen.c from the
// info_node entities, like CGraph in dlls/nodes.cpp) and A* routes over it.
#include <libdragon.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "game.h"
#include "nav.h"

typedef struct { float o[3]; uint8_t air, pad; uint16_t first, count, pad2; } navnode_t;
typedef struct { uint16_t dest; uint8_t hulls, pad; int16_t door; uint16_t pad2; } navlink_t;
_Static_assert(sizeof(navnode_t) == 20, "node size");
_Static_assert(sizeof(navlink_t) == 8, "link size");

static uint8_t *navbuf;
static navnode_t *nodes;
static navlink_t *links;
static int numnodes;

void nav_load(const char *map)
{
    free(navbuf);
    navbuf = NULL;
    numnodes = 0;
    if (!map) return;   // just free (before a level load, so the buffer doesn't split the heap)
    char path[64];
    snprintf(path, sizeof(path), "rom:/maps/%s.nav", map);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    int len = ftell(f);
    fseek(f, 0, SEEK_SET);
    navbuf = malloc(len);
    fread(navbuf, 1, len, f);
    fclose(f);
    if (memcmp(navbuf, "HLN1", 4)) { free(navbuf); navbuf = NULL; return; }
    numnodes = (navbuf[4] << 8) | navbuf[5];
    nodes = (navnode_t *)(navbuf + 8);
    links = (navlink_t *)(navbuf + 8 + numnodes * sizeof(navnode_t));
    debugf("nav: %d nodes\n", numnodes);
}

int nav_num_nodes(void) { return numnodes; }
const float *nav_node_origin(int n) { return nodes[n].o; }

// is a link usable right now? (doors: open, or one a monster could open by walking into it)
static bool link_ok(navlink_t *l, int hull)
{
    if (!(l->hulls & (1 << hull))) return false;
    if (l->door > 0) {
        for (int i = 1; i < num_entities; i++) {
            entity_t *e = &entities[i];
            if (e->model != l->door) continue;
            if (e->removed || e->solid != SOLID_BSP) return true;
            bool up = e->state == MS_AT_TOP || e->state == MS_GOING_UP;
            // a START_OPEN door (the c1a3 fire door) is open at "bottom" and shut at "top"
            if (e->spawnflags & 1) return !up;
            if (up) return true;
            // doors without a name open when touched (monsters with CAP_OPEN_DOORS)
            return !e->targetname[0] && !(e->spawnflags & 256);
        }
    }
    return true;
}

static float d3(const float *a, const float *b)
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}

// nearest nodes a monster at `p` can walk to directly
int nav_nearest(const float *p, int hull, bool air, bool (*reach)(const float *from, const float *to, void *ctx), void *ctx)
{
    int cand[8];
    float cd[8];
    int nc = 0;
    for (int i = 0; i < numnodes; i++) {
        if ((nodes[i].air != 0) != air) continue;
        float d = d3(p, nodes[i].o);
        if (d > 1500) continue;
        int j = nc < 8 ? nc++ : 8;
        if (j == 8) { if (d >= cd[7]) continue; j = 7; }
        while (j > 0 && cd[j - 1] > d) { cand[j] = cand[j - 1]; cd[j] = cd[j - 1]; j--; }
        cand[j] = i; cd[j] = d;
    }
    for (int k = 0; k < nc; k++)
        if (!reach || reach(p, nodes[cand[k]].o, ctx)) return cand[k];
    return -1;
}

// A* from node a to node b; writes the node list (a .. b) to out, returns its length (0 = no route)
int nav_route(int a, int b, int hull, int *out, int max)
{
    if (a < 0 || b < 0 || !numnodes) return 0;
    if (a == b) { out[0] = a; return 1; }
    static float g[2048], h[2048];
    static int16_t from[2048];
    static uint8_t state[2048];   // 0 new, 1 open, 2 closed
    static int16_t open[2048];    // the open set, so each step scans only it (not every node)
    int n = numnodes > 2048 ? 2048 : numnodes;
    memset(state, 0, n);
    g[a] = 0;
    h[a] = d3(nodes[a].o, nodes[b].o);
    from[a] = -1;
    state[a] = 1;
    int nopen = 0;
    open[nopen++] = a;
    while (nopen > 0) {
        int bi = 0;
        float bf = g[open[0]] + h[open[0]];
        for (int k = 1; k < nopen; k++) {
            float f = g[open[k]] + h[open[k]];
            if (f < bf) { bf = f; bi = k; }
        }
        int best = open[bi];
        if (best == b) break;
        open[bi] = open[--nopen];
        state[best] = 2;
        navnode_t *nd = &nodes[best];
        for (int k = 0; k < nd->count; k++) {
            navlink_t *l = &links[nd->first + k];
            int t = l->dest;
            if (t >= n || state[t] == 2 || !link_ok(l, hull)) continue;
            float ng = g[best] + d3(nd->o, nodes[t].o);
            if (state[t] == 0) { h[t] = d3(nodes[t].o, nodes[b].o); g[t] = ng; from[t] = best; state[t] = 1; open[nopen++] = t; }
            else if (ng < g[t]) { g[t] = ng; from[t] = best; }
        }
    }
    if (state[b] != 1 && state[b] != 2) return 0;
    int tmp[256], c = 0;
    for (int x = b; x >= 0 && c < 256; x = from[x]) tmp[c++] = x;
    if (c > max) c = max;
    for (int i = 0; i < c; i++) out[i] = tmp[c - 1 - i];
    return c;
}
