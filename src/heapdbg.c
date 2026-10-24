// test builds only (-DHEAPDBG, link with --wrap): track live allocations to find what
// fragments the heap between maps
#ifdef HEAPDBG
#include <libdragon.h>
#include <malloc.h>
#include <reent.h>
void *__real_malloc(size_t);
void __real_free(void *);
void *__real_memalign(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__real_calloc(size_t, size_t);
typedef struct { void *p; size_t n; void *ra; } rec_t;
#define NR 8192
static rec_t recs[NR];
static void add(void *p, size_t n, void *ra) { if (!p || n < 32) return; for (int i = 0; i < NR; i++) if (!recs[i].p) { recs[i] = (rec_t){p, n, ra}; return; } }
static void del(void *p) { if (!p) return; for (int i = 0; i < NR; i++) if (recs[i].p == p) { recs[i].p = NULL; return; } }
void *__wrap_malloc(size_t n) { void *p = __real_malloc(n); add(p, n, __builtin_return_address(0)); return p; }
void *__wrap_memalign(size_t a, size_t n) { void *p = __real_memalign(a, n); add(p, n, __builtin_return_address(0)); return p; }
void *__wrap_calloc(size_t a, size_t n) { void *p = __real_calloc(a, n); add(p, a * n, __builtin_return_address(0)); return p; }
void *__wrap_realloc(void *o, size_t n) { del(o); void *p = __real_realloc(o, n); add(p, n, __builtin_return_address(0)); return p; }
void __wrap_free(void *p) { del(p); __real_free(p); }
void *__real__malloc_r(struct _reent *, size_t);
void __real__free_r(struct _reent *, void *);
void *__wrap__malloc_r(struct _reent *r, size_t n) { void *p = __real__malloc_r(r, n); add(p, n, __builtin_return_address(0)); return p; }
void __wrap__free_r(struct _reent *r, void *p) { del(p); __real__free_r(r, p); }
void *__real__calloc_r(struct _reent *, size_t, size_t);
void *__wrap__calloc_r(struct _reent *r, size_t a, size_t n) { void *p = __real__calloc_r(r, a, n); add(p, a * n + 1, __builtin_return_address(0)); return p; }
void heapdbg_dump(void)
{
    for (int i = 0; i < NR; i++) if (recs[i].p) debugf("LIVE %p %d B from %p\n", recs[i].p, (int)recs[i].n, recs[i].ra);
}
#endif
