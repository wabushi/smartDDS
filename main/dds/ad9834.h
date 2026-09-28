#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum { AD9834_WAVE_SINE, AD9834_WAVE_TRIANGLE, AD9834_WAVE_SQUARE } ad9834_waveform_t;

esp_err_t ad9834_init(uint32_t mclk_hz);
esp_err_t ad9834_set_mclk(uint32_t mclk_hz);
esp_err_t ad9834_set_frequency(uint8_t reg, uint32_t hz);
esp_err_t ad9834_set_phase(uint8_t reg, float degrees);
esp_err_t ad9834_select_frequency(uint8_t reg);
esp_err_t ad9834_select_phase(uint8_t reg);
esp_err_t ad9834_set_waveform(ad9834_waveform_t waveform);
esp_err_t ad9834_set_output(bool enabled);
esp_err_t ad9834_reset(void);
uint32_t ad9834_frequency_word(uint32_t hz, uint32_t mclk_hz);
