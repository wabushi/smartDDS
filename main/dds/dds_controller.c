#include "dds_controller.h"
#include "ad9834.h"
#include "config/dds_config.h"
#include "display/display.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>

static const char *TAG = "DDS";
static dds_state_t s_state;
static SemaphoreHandle_t s_lock;

static esp_err_t notify_state_change(esp_err_t err)
{
    if (err == ESP_OK) {
        display_notify_state_change();
    }
    return err;
}

static bool valid_output(dds_output_t output) { return output < DDS_OUTPUT_COUNT; }

static esp_err_t apply_profile_locked(dds_output_t output)
{
    const dds_output_state_t *p = &s_state.outputs[output];
    ESP_RETURN_ON_ERROR(ad9834_set_frequency(0, p->frequency0_hz), TAG, "frequency 0");
    ESP_RETURN_ON_ERROR(ad9834_set_frequency(1, p->frequency1_hz), TAG, "frequency 1");
    ESP_RETURN_ON_ERROR(ad9834_set_phase(0, p->phase0_deg), TAG, "phase 0");
    ESP_RETURN_ON_ERROR(ad9834_set_phase(1, p->phase1_deg), TAG, "phase 1");
    ESP_RETURN_ON_ERROR(ad9834_select_frequency(p->active_frequency_register), TAG, "frequency select");
    ESP_RETURN_ON_ERROR(ad9834_select_phase(p->active_phase_register), TAG, "phase select");
    ESP_RETURN_ON_ERROR(ad9834_set_waveform((ad9834_waveform_t)p->waveform), TAG, "waveform");
    return ad9834_set_output(p->output_enabled);
}

static esp_err_t apply_if_active_locked(dds_output_t output)
{
    return output == s_state.active_output ? apply_profile_locked(output) : ESP_OK;
}

void dds_controller_get_state(dds_state_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

esp_err_t dds_controller_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    dds_state_init(&s_state, (uint32_t)AD9834_MCLK_HZ);
    ESP_RETURN_ON_ERROR(ad9834_init(s_state.mclk_hz), TAG, "driver init");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = apply_profile_locked(s_state.active_output);
    if (err == ESP_OK) s_state.hardware_initialized = true;
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t dds_controller_select_output(dds_output_t output)
{
    if (!valid_output(output)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = apply_profile_locked(output);
    if (err == ESP_OK) s_state.active_output = output;
    xSemaphoreGive(s_lock);
    return notify_state_change(err);
}

esp_err_t dds_controller_set_frequency_for_output(dds_output_t output, uint8_t reg, uint32_t hz)
{
    if (!valid_output(output) || reg > 1 || hz == 0 || hz > AD9834_MAX_OUTPUT_HZ) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    dds_output_state_t *p = &s_state.outputs[output];
    uint32_t *dst = reg ? &p->frequency1_hz : &p->frequency0_hz;
    uint32_t old = *dst;
    *dst = hz;
    esp_err_t err = apply_if_active_locked(output);
    if (err != ESP_OK) *dst = old;
    xSemaphoreGive(s_lock);
    return notify_state_change(err);
}

esp_err_t dds_controller_select_frequency_for_output(dds_output_t output, uint8_t reg)
{
    if (!valid_output(output) || reg > 1) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.outputs[output].active_frequency_register = reg;
    esp_err_t err = apply_if_active_locked(output);
    xSemaphoreGive(s_lock);
    return notify_state_change(err);
}

esp_err_t dds_controller_set_phase_for_output(dds_output_t output, uint8_t reg, float degrees)
{
    if (!valid_output(output) || reg > 1 || !isfinite(degrees) || degrees < 0.0f || degrees >= 360.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    dds_output_state_t *p = &s_state.outputs[output];
    float old = reg ? p->phase1_deg : p->phase0_deg;
    (reg ? &p->phase1_deg : &p->phase0_deg)[0] = degrees;
    esp_err_t err = apply_if_active_locked(output);
    if (err != ESP_OK) (reg ? &p->phase1_deg : &p->phase0_deg)[0] = old;
    xSemaphoreGive(s_lock);
    return notify_state_change(err);
}

esp_err_t dds_controller_select_phase_for_output(dds_output_t output, uint8_t reg)
{
    if (!valid_output(output) || reg > 1) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.outputs[output].active_phase_register = reg;
    esp_err_t err = apply_if_active_locked(output);
    xSemaphoreGive(s_lock);
    return notify_state_change(err);
}

esp_err_t dds_controller_set_waveform_for_output(dds_output_t output, dds_waveform_t waveform)
{
    if (!valid_output(output) || waveform > DDS_WAVE_SQUARE) return ESP_ERR_INVALID_ARG;
    if (output == DDS_OUTPUT_SQUARE && waveform != DDS_WAVE_SQUARE) return ESP_ERR_INVALID_ARG;
    if (output != DDS_OUTPUT_SQUARE && waveform == DDS_WAVE_SQUARE) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    dds_waveform_t old = s_state.outputs[output].waveform;
    s_state.outputs[output].waveform = waveform;
    esp_err_t err = apply_if_active_locked(output);
    if (err != ESP_OK) s_state.outputs[output].waveform = old;
    xSemaphoreGive(s_lock);
    return notify_state_change(err);
}

esp_err_t dds_controller_set_output_for_output(dds_output_t output, bool enabled)
{
    if (!valid_output(output)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool old = s_state.outputs[output].output_enabled;
    s_state.outputs[output].output_enabled = enabled;
    esp_err_t err = apply_if_active_locked(output);
    if (err != ESP_OK) s_state.outputs[output].output_enabled = old;
    if (err == ESP_OK && output == s_state.active_output) s_state.reset = !enabled;
    xSemaphoreGive(s_lock);
    return notify_state_change(err);
}

static dds_output_t active_output(void)
{
    dds_state_t copy;
    dds_controller_get_state(&copy);
    return copy.active_output;
}

esp_err_t dds_controller_set_frequency(uint8_t reg, uint32_t hz) { return dds_controller_set_frequency_for_output(active_output(), reg, hz); }
esp_err_t dds_controller_select_frequency(uint8_t reg) { return dds_controller_select_frequency_for_output(active_output(), reg); }
esp_err_t dds_controller_set_phase(uint8_t reg, float degrees) { return dds_controller_set_phase_for_output(active_output(), reg, degrees); }
esp_err_t dds_controller_select_phase(uint8_t reg) { return dds_controller_select_phase_for_output(active_output(), reg); }
esp_err_t dds_controller_set_waveform(dds_waveform_t waveform) { return dds_controller_set_waveform_for_output(active_output(), waveform); }
esp_err_t dds_controller_set_output(bool enabled) { return dds_controller_set_output_for_output(active_output(), enabled); }

esp_err_t dds_controller_reset(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ad9834_reset();
    if (err == ESP_OK) {
        s_state.reset = true;
        for (int i = 0; i < DDS_OUTPUT_COUNT; ++i) s_state.outputs[i].output_enabled = false;
    }
    xSemaphoreGive(s_lock);
    return notify_state_change(err);
}

esp_err_t dds_controller_set_mclk(uint32_t hz)
{
    if (!hz || hz > 100000000U) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ad9834_set_mclk(hz);
    if (err == ESP_OK) {
        s_state.mclk_hz = hz;
        err = apply_profile_locked(s_state.active_output);
    }
    xSemaphoreGive(s_lock);
    return notify_state_change(err);
}
