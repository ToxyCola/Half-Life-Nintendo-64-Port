// PC harness only: byte-swap a big-endian .hlb in place so the game code can read it on x86.
#include <stdint.h>
#include <string.h>
#include "bsp.h"
static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }
static void sw32(void *p, int n) { uint32_t *q = p; for (int i = 0; i < n; i++) q[i] = be32(q[i]); }
static void sw16(void *p, int n) { uint16_t *q = p; for (int i = 0; i < n; i++) q[i] = __builtin_bswap16(q[i]); }
void host_swap_hlb(uint8_t *buf)
{
    uint32_t *h = (uint32_t *)buf;
    sw32(h + 1, 3);
    int n = h[3];
    uint32_t *tab = h + 4;
    sw32(tab, n * 2);
#define L(i) (buf + tab[(i) * 2])
#define C(i) (tab[(i) * 2 + 1])
    for (uint32_t i = 0; i < C(0); i++) sw32(L(0) + i * 20, 4);                       // planes
    sw32(L(1), C(1) * 3);                                                             // clipnodes
    for (uint32_t i = 0; i < C(2); i++) { sw32(L(2) + i * 24, 1); sw16(L(2) + i * 24 + 4, 10); }  // nodes
    for (uint32_t i = 0; i < C(3); i++) { sw32(L(3) + i * 28, 2); sw16(L(3) + i * 28 + 8, 8); }   // leafs
    sw16(L(4), C(4));                                                                 // marks
    sw32(L(6), C(6) * 16);                                                            // models
    for (uint32_t i = 0; i < C(7); i++) { uint8_t *f = L(7) + i * 16; sw16(f, 1); sw16(f + 4, 2); sw32(f + 8, 1); sw16(f + 12, 2); }
    sw32(L(8), C(7) * 13);                                                            // facetex
    for (uint32_t i = 0; i < C(9); i++) { uint8_t *c = L(9) + i * 12; sw32(c, 1); sw16(c + 6, 1); sw32(c + 8, 1); }
    for (uint32_t i = 0; i < C(10) / 2; i++) { uint8_t *v = L(10) + i * 32; sw16(v, 8); sw16(v + 24, 4); }
    for (uint32_t i = 0; i < C(12); i++) { uint8_t *t = L(12) + i * 36; sw16(t + 16, 6); sw32(t + 28, 2); }
}
