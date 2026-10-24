#pragma once
#include <stdbool.h>
void nav_load(const char *map);
int  nav_num_nodes(void);
const float *nav_node_origin(int n);
int  nav_nearest(const float *p, int hull, bool air, bool (*reach)(const float *from, const float *to, void *ctx), void *ctx);
int  nav_route(int a, int b, int hull, int *out, int max);
