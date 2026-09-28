#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t display_hw_init(void);
bool display_hw_present(void);
esp_err_t display_hw_show_text(const char *line1, const char *line2, const char *line3);
esp_err_t display_hw_show_bitmap(const uint8_t *bitmap);

typedef enum {
    DISPLAY_WAVE_SINE = 0,
    DISPLAY_WAVE_TRIANGLE,
    DISPLAY_WAVE_SQUARE,
} display_waveform_t;

esp_err_t display_hw_show_output(const char *output_id, const char *frequency,
                                 const char *output_state, const char *phase,
                                 const char *waveform_name, display_waveform_t waveform,
                                 uint32_t waveform_offset);
esp_err_t display_hw_animate_output(display_waveform_t waveform, uint32_t waveform_offset);
esp_err_t display_hw_fill(bool on);
