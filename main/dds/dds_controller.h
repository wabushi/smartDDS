#pragma once
#include "esp_err.h"
#include "dds_state.h"

esp_err_t dds_controller_init(void);
void dds_controller_get_state(dds_state_t *out);
esp_err_t dds_controller_select_output(dds_output_t output);
esp_err_t dds_controller_set_frequency_for_output(dds_output_t output, uint8_t reg, uint32_t hz);
esp_err_t dds_controller_select_frequency_for_output(dds_output_t output, uint8_t reg);
esp_err_t dds_controller_set_phase_for_output(dds_output_t output, uint8_t reg, float degrees);
esp_err_t dds_controller_select_phase_for_output(dds_output_t output, uint8_t reg);
esp_err_t dds_controller_set_waveform_for_output(dds_output_t output, dds_waveform_t waveform);
esp_err_t dds_controller_set_output_for_output(dds_output_t output, bool enabled);
esp_err_t dds_controller_set_frequency(uint8_t reg, uint32_t hz);
esp_err_t dds_controller_select_frequency(uint8_t reg);
esp_err_t dds_controller_set_phase(uint8_t reg, float degrees);
esp_err_t dds_controller_select_phase(uint8_t reg);
esp_err_t dds_controller_set_waveform(dds_waveform_t waveform);
esp_err_t dds_controller_set_output(bool enabled);
esp_err_t dds_controller_reset(void);
esp_err_t dds_controller_set_mclk(uint32_t hz);
