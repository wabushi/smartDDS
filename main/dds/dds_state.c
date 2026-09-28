#include "dds_state.h"

void dds_state_init(dds_state_t *s, uint32_t mclk_hz)
{
    *s = (dds_state_t){ .active_output = DDS_OUTPUT_SINE1, .mclk_hz = mclk_hz };
    for (int i = 0; i < DDS_OUTPUT_COUNT; ++i) {
        s->outputs[i].frequency0_hz = 1000000;
        s->outputs[i].waveform = (i == DDS_OUTPUT_SQUARE) ? DDS_WAVE_SQUARE : DDS_WAVE_SINE;
        // Start in the documented 1 MHz sine test state with the DDS output
        // enabled. Leaving this false keeps the AD9834 in reset/sleep and
        // produces no waveform until a transport command is sent.
        s->outputs[i].output_enabled = true;
    }
}
const char *dds_waveform_name(dds_waveform_t w)
{
    return w == DDS_WAVE_TRIANGLE ? "triangle" : w == DDS_WAVE_SQUARE ? "square" : "sine";
}

const char *dds_output_name(dds_output_t output)
{
    return output == DDS_OUTPUT_SINE1 ? "s1" : output == DDS_OUTPUT_SINE2 ? "s2" : "s3";
}
