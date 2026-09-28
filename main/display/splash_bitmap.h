#pragma once

#include <stdint.h>

#define SPLASH_BITMAP_WIDTH 72U
#define SPLASH_BITMAP_HEIGHT 40U
#define SPLASH_BITMAP_BYTES ((SPLASH_BITMAP_WIDTH * SPLASH_BITMAP_HEIGHT) / 8U)

extern const uint8_t splash_bitmap[SPLASH_BITMAP_BYTES];
