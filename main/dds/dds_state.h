#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { DDS_WAVE_SINE, DDS_WAVE_TRIANGLE, DDS_WAVE_SQUARE } dds_waveform_t;
typedef enum {
    DDS_OUTPUT_SINE1 = 0,
    DDS_OUTPUT_SINE2,
    DDS_OUTPUT_SQUARE,
    DDS_OUTPUT_COUNT
} dds_output_t;

typedef struct {
    uint32_t frequency0_hz;
    uint32_t frequency1_hz;
    uint8_t active_frequency_register;
    float phase0_deg;
    float phase1_deg;
    uint8_t active_phase_register;
    dds_waveform_t waveform;
    bool output_enabled;
} dds_output_state_t;

typedef struct {
    dds_output_state_t outputs[DDS_OUTPUT_COUNT];
    dds_output_t active_output;
    bool reset;
    bool hardware_initialized;
    uint32_t mclk_hz;
} dds_state_t;

void dds_state_init(dds_state_t *state, uint32_t mclk_hz);
const char *dds_waveform_name(dds_waveform_t waveform);
const char *dds_output_name(dds_output_t output);
