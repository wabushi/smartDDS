#include "measurement.h"
#include <stdio.h>

void measurement_get(dds_measurements_t *m) { *m = (dds_measurements_t){0}; }
int measurement_format(const dds_measurements_t *m, char *b, size_t n)
{
    (void)m; return snprintf(b,n,"DDS Measurements\nSupply voltage: unavailable\nSine1 voltage: unavailable\nSine2 voltage: unavailable\nFrequency: unavailable\nVpp: unavailable\nVrms: unavailable\nDC offset: unavailable\n");
}
