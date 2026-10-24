#pragma once
#include <stdint.h>
typedef struct { int16_t posA[3]; uint16_t normA; int16_t posB[3]; uint16_t normB; uint32_t rgbaA, rgbaB; int16_t stA[2], stB[2]; } T3DVertPacked;
