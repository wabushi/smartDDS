#include "display_ui.h"
#include "dds/dds_controller.h"
#include "dds/dds_state.h"
#include "esp_log.h"
#include <stdio.h>
#include "display_hw.h"

void display_ui_render(display_output_t output)
{
    display_ui_render_animated(output, 0);
}

void display_ui_render_animated(display_output_t output, uint32_t waveform_offset)
{
    dds_state_t s; dds_controller_get_state(&s);
    const char *id = output == DISPLAY_SINE1 ? "S1" : output == DISPLAY_SINE2 ? "S2" : "S3";
    const char *name = output == DISPLAY_SINE1 ? "SINE1" : output == DISPLAY_SINE2 ? "SINE2" : "SQUARE";
    const dds_output_state_t *state = &s.outputs[(dds_output_t)output];
    uint32_t frequency_hz = state->active_frequency_register ? state->frequency1_hz : state->frequency0_hz;
    char frequency[16];
    char phase[16];
    /* The small reference display has a fixed MHz presentation.  Keep the
     * programmed value in Hz in the state model, but show two decimals here. */
    snprintf(frequency, sizeof(frequency), "%.2fMHZ",
             (double)frequency_hz / 1000000.0);
    /* Four glyphs fit completely in the left column without touching the
     * animated waveform area. The authoritative phase remains a float. */
    unsigned phase_deg = (unsigned)((state->active_phase_register ? state->phase1_deg : state->phase0_deg) + 0.5f) % 360U;
    snprintf(phase, sizeof(phase), "P%03u", phase_deg);
    const dds_waveform_t configured_waveform =
        output == DISPLAY_SQUARE ? DDS_WAVE_SQUARE : state->waveform;
    const display_waveform_t display_waveform =
        configured_waveform == DDS_WAVE_TRIANGLE ? DISPLAY_WAVE_TRIANGLE :
        configured_waveform == DDS_WAVE_SQUARE ? DISPLAY_WAVE_SQUARE : DISPLAY_WAVE_SINE;
    const char *physical_wave = dds_waveform_name(configured_waveform);
    const char *waveform_name = display_waveform == DISPLAY_WAVE_TRIANGLE ? "TRI" :
                                display_waveform == DISPLAY_WAVE_SQUARE ? "SQUARE" : "SINE";
    /* The graphic identifies the configured waveform without pretending it
     * is a measured trace. */
    ESP_LOGD("DISPLAY", "%s (%s) configured: freq=%lu Hz waveform=%s output=%s",
             name, physical_wave, (unsigned long)frequency_hz,
             physical_wave, state->output_enabled ? "ON" : "OFF");
    if (display_hw_present()) {
        if (waveform_offset == 0) {
            display_hw_show_output(id, frequency, state->output_enabled ? "ON" : "OFF", phase,
                                   waveform_name, display_waveform, waveform_offset);
        } else {
            display_hw_animate_output(display_waveform, waveform_offset);
        }
    }
}
