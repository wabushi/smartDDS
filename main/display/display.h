#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef enum { DISPLAY_SINE1, DISPLAY_SINE2, DISPLAY_SQUARE } display_output_t;
esp_err_t display_init(void);
void display_next(void); void display_select(display_output_t output);
void display_set_rotation(bool enabled); bool display_rotation_enabled(void);
esp_err_t display_set_interval(uint32_t seconds); uint32_t display_get_interval(void);
display_output_t display_get_output(void);
int display_status(char *buf, size_t len);
void display_notify_state_change(void);
