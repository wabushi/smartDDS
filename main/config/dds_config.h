#pragma once

#include <stdint.h>

#define AD9834_MCLK_HZ 75000000ULL
#define AD9834_SPI_HZ  1000000U
#define AD9834_FREQ_BITS 28U
#define AD9834_MAX_OUTPUT_HZ (AD9834_MCLK_HZ / 2ULL)
