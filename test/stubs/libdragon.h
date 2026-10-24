// Minimal host stand-ins for the libdragon APIs used by the game code (PC test harness only)
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <malloc.h>
#define debugf(...) fprintf(stderr, __VA_ARGS__)
static inline void data_cache_hit_writeback(void *p, int n) { (void)p; (void)n; }
#define TICKS_READ() 0
#define TICKS_PER_SECOND 1
FILE *host_fopen(const char *path, const char *mode);
#define fopen host_fopen
static inline uint64_t get_ticks_ms(void) { return 0; }
static inline void *asset_load(const char *path, int *size)
{
    FILE *f = host_fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); int n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = memalign(16, n); fread(b, 1, n, f); fclose(f); if (size) *size = n; return b;
}
void host_swap_hlb(uint8_t *buf);
