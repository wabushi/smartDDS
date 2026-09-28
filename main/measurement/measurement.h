#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool supply_voltage_valid; float supply_voltage;
    bool sine1_voltage_valid; float sine1_voltage;
    bool sine2_voltage_valid; float sine2_voltage;
    bool frequency_valid; float measured_frequency_hz;
    bool vpp_valid; float vpp;
    bool vrms_valid; float vrms;
    bool dc_offset_valid; float dc_offset;
} dds_measurements_t;
void measurement_get(dds_measurements_t *out);
int measurement_format(const dds_measurements_t *m, char *buf, size_t len);
