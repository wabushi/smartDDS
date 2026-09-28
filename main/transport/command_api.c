#include "command_api.h"
#include "dds/dds_controller.h"
#include "dds/dds_state.h"
#include "display/display.h"
#include "measurement/measurement.h"
#include "network/wifi_manager.h"
#include "esp_log.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static command_notify_fn_t s_notify;

void command_api_set_notify(command_notify_fn_t fn) { s_notify = fn; }
static void changed(void) { if (s_notify) s_notify(); }
static int fail(char *b, size_t n, const char *msg) { return snprintf(b, n, "ERROR %s\n", msg); }

static const char *wave_name(dds_waveform_t w)
{
    return w == DDS_WAVE_TRIANGLE ? "triangle" : w == DDS_WAVE_SQUARE ? "square" : "sine";
}

static const char *output_name(dds_output_t output)
{
    return output == DDS_OUTPUT_SINE2 ? "s2" : output == DDS_OUTPUT_SQUARE ? "s3" : "s1";
}

static bool parse_output_name(const char *value, dds_output_t *output)
{
    if (!value || !output) return false;
    if (!strcasecmp(value, "s1") || !strcasecmp(value, "sine1")) *output = DDS_OUTPUT_SINE1;
    else if (!strcasecmp(value, "s2") || !strcasecmp(value, "sine2")) *output = DDS_OUTPUT_SINE2;
    else if (!strcasecmp(value, "s3") || !strcasecmp(value, "square")) *output = DDS_OUTPUT_SQUARE;
    else return false;
    return true;
}

static int state_text(char *b, size_t n)
{
    dds_state_t s;
    char ssid[33], ip[16], server[128];
    wifi_manager_get_status(ssid, sizeof(ssid), ip, sizeof(ip));
    wifi_manager_get_server_url(server, sizeof(server));
    dds_controller_get_state(&s);
    int used = snprintf(b, n, "DDS STATE\nHardware: %s\nActive output: %s\nMCLK: %lu Hz\n",
                        s.hardware_initialized ? "initialized" : "not initialized",
                        output_name(s.active_output), (unsigned long)s.mclk_hz);
    for (int i = 0; i < DDS_OUTPUT_COUNT && used > 0 && (size_t)used < n; ++i) {
        const dds_output_state_t *p = &s.outputs[i];
        uint32_t f = p->active_frequency_register ? p->frequency1_hz : p->frequency0_hz;
        used += snprintf(b + used, n - (size_t)used,
                         "%s: %lu Hz, %s, phase %.2f deg, OUT %s\n",
                         output_name((dds_output_t)i), (unsigned long)f, wave_name(p->waveform),
                         p->active_phase_register ? p->phase1_deg : p->phase0_deg,
                         p->output_enabled ? "ON" : "OFF");
    }
    if (used > 0 && (size_t)used < n) {
        used += snprintf(b + used, n - (size_t)used,
                         "Wi-Fi: %s\nSSID: %s\nIP: %s\nServer: %s\nMeasurements:\n"
                         "Supply voltage: unavailable\nSine1 voltage: unavailable\n"
                         "Sine2 voltage: unavailable\nMeasured frequency: unavailable\n",
                         wifi_manager_is_connected() ? "connected" : wifi_manager_is_configured() ? "configured; connecting" : "not configured",
                         ssid[0] ? ssid : "-", ip[0] ? ip : "-", server[0] ? server : "-");
    }
    return used;
}

int command_api_get_state_json(char *b, size_t n)
{
    dds_state_t s;
    char ssid[33], ip[16], server[128];
    wifi_manager_get_status(ssid, sizeof(ssid), ip, sizeof(ip));
    wifi_manager_get_server_url(server, sizeof(server));
    dds_controller_get_state(&s);
    int used = snprintf(b, n, "{\"type\":\"state\",\"hardware_initialized\":%s,\"reset\":%s,\"active_output\":\"%s\",\"mclk_hz\":%lu,\"outputs\":[",
                        s.hardware_initialized ? "true" : "false", s.reset ? "true" : "false",
                        output_name(s.active_output), (unsigned long)s.mclk_hz);
    for (int i = 0; i < DDS_OUTPUT_COUNT && used > 0 && (size_t)used < n; ++i) {
        const dds_output_state_t *p = &s.outputs[i];
        used += snprintf(b + used, n - (size_t)used,
                         "%s{\"id\":\"%s\",\"frequency0_hz\":%lu,\"frequency1_hz\":%lu,\"active_frequency_register\":%u,\"phase0_deg\":%.2f,\"phase1_deg\":%.2f,\"active_phase_register\":%u,\"waveform\":\"%s\",\"output_enabled\":%s}",
                         i ? "," : "", output_name((dds_output_t)i), (unsigned long)p->frequency0_hz,
                         (unsigned long)p->frequency1_hz, p->active_frequency_register, p->phase0_deg,
                         p->phase1_deg, p->active_phase_register, wave_name(p->waveform),
                         p->output_enabled ? "true" : "false");
    }
    if (used > 0 && (size_t)used < n) {
        used += snprintf(b + used, n - (size_t)used,
                         "],\"wifi\":{\"configured\":%s,\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"server_url\":\"%s\"},\"measurements\":{\"supply_voltage\":null,\"sine1_vpp\":null,\"sine2_vpp\":null,\"measured_frequency_hz\":null}}",
                         wifi_manager_is_configured() ? "true" : "false", wifi_manager_is_connected() ? "true" : "false",
                         ssid, ip, server);
    }
    return used;
}

static bool parse_hz(const char *input, uint32_t *out)
{
    char tmp[40]; size_t len; char *end; double mult = 1.0, value;
    if (!input || !out) return false;
    len = strlen(input); while (len && isspace((unsigned char)input[len - 1])) --len;
    if (!len || len >= sizeof(tmp)) return false;
    memcpy(tmp, input, len); tmp[len] = 0;
    if (len >= 2 && (tmp[len - 1] == 'z' || tmp[len - 1] == 'Z')) tmp[--len] = 0;
    if (len >= 1 && (tmp[len - 1] == 'h' || tmp[len - 1] == 'H')) tmp[--len] = 0;
    if (len >= 1 && (tmp[len - 1] == 'k' || tmp[len - 1] == 'K')) mult = 1e3, tmp[--len] = 0;
    else if (len >= 1 && (tmp[len - 1] == 'm' || tmp[len - 1] == 'M')) mult = 1e6, tmp[--len] = 0;
    else if (len >= 1 && (tmp[len - 1] == 'g' || tmp[len - 1] == 'G')) mult = 1e9, tmp[--len] = 0;
    errno = 0; value = strtod(tmp, &end);
    if (errno || end == tmp || *end || !isfinite(value) || value < 0 || value * mult > 4294967295.0) return false;
    *out = (uint32_t)llround(value * mult); return true;
}

static bool parse_float(const char *input, float *out)
{
    char *end; float value;
    if (!input || !out) return false;
    errno = 0; value = strtof(input, &end);
    if (errno || end == input || *end || !isfinite(value)) return false;
    *out = value; return true;
}

static int finish(esp_err_t err, char *b, size_t n, const char *ok)
{
    if (err != ESP_OK) return fail(b, n, "invalid value or hardware write failed");
    changed(); return snprintf(b, n, "OK %s\n", ok);
}

static dds_output_t cli_output(void) { dds_state_t s; dds_controller_get_state(&s); return s.active_output; }

int command_api_execute_line(const char *line, char *b, size_t n)
{
    char copy[200], *save, *a, *v; dds_output_t output; uint32_t hz; float degrees;
    if (!line || strlen(line) >= sizeof(copy)) return fail(b, n, "command too long");
    strcpy(copy, line); a = strtok_r(copy, " \t\r\n", &save); if (!a) return 0;
    if (!strcmp(a, "help")) return snprintf(b, n, "help status select-output freq freq0 freq1 select-freq phase phase0 phase1 select-phase wave output reset mclk measure wifi display\n");
    if (!strcmp(a, "status")) return state_text(b, n);
    if (!strcmp(a, "measure")) { dds_measurements_t m; measurement_get(&m); return measurement_format(&m, b, n); }
    if (!strcmp(a, "select-output")) { v = strtok_r(NULL, " \t\r\n", &save); return parse_output_name(v, &output) ? finish(dds_controller_select_output(output), b, n, "output selected") : fail(b, n, "output must be s1, s2, or s3"); }
    output = cli_output();
    if (!strcmp(a, "freq") || !strcmp(a, "freq0") || !strcmp(a, "freq1")) { v = strtok_r(NULL, " \t\r\n", &save); if (!parse_hz(v, &hz)) return fail(b, n, "invalid frequency"); return finish(dds_controller_set_frequency_for_output(output, !strcmp(a, "freq1"), hz), b, n, "frequency updated"); }
    if (!strcmp(a, "phase") || !strcmp(a, "phase0") || !strcmp(a, "phase1")) { v = strtok_r(NULL, " \t\r\n", &save); if (!parse_float(v, &degrees) || degrees < 0 || degrees >= 360) return fail(b, n, "phase must be 0 through 359.999 degrees"); return finish(dds_controller_set_phase_for_output(output, !strcmp(a, "phase1"), degrees), b, n, "phase updated"); }
    if (!strcmp(a, "select-freq") || !strcmp(a, "select-phase")) { v = strtok_r(NULL, " \t\r\n", &save); return (v && (*v == '0' || *v == '1')) ? finish(!strcmp(a, "select-freq") ? dds_controller_select_frequency_for_output(output, (uint8_t)(*v - '0')) : dds_controller_select_phase_for_output(output, (uint8_t)(*v - '0')), b, n, "register selected") : fail(b, n, "register must be 0 or 1"); }
    if (!strcmp(a, "wave")) { dds_waveform_t w; v = strtok_r(NULL, " \t\r\n", &save); if (!v) return fail(b, n, "waveform required"); w = !strcasecmp(v, "sine") ? DDS_WAVE_SINE : !strcasecmp(v, "triangle") ? DDS_WAVE_TRIANGLE : !strcasecmp(v, "square") ? DDS_WAVE_SQUARE : 99; return finish(dds_controller_set_waveform_for_output(output, w), b, n, "waveform updated"); }
    if (!strcmp(a, "output")) { v = strtok_r(NULL, " \t\r\n", &save); return finish(v && (!strcasecmp(v, "on") || !strcasecmp(v, "off")) ? dds_controller_set_output_for_output(output, !strcasecmp(v, "on")) : ESP_ERR_INVALID_ARG, b, n, "output updated"); }
    if (!strcmp(a, "reset")) return finish(dds_controller_reset(), b, n, "reset asserted; outputs disabled");
    if (!strcmp(a, "mclk")) { v = strtok_r(NULL, " \t\r\n", &save); if (!v) { dds_state_t s; dds_controller_get_state(&s); return snprintf(b, n, "MCLK: %lu Hz\n", (unsigned long)s.mclk_hz); } return finish(parse_hz(v, &hz) ? dds_controller_set_mclk(hz) : ESP_ERR_INVALID_ARG, b, n, "MCLK updated"); }
    if (!strcmp(a, "wifi")) { v = strtok_r(NULL, " \t\r\n", &save); if (!v || !strcmp(v, "status")) { char ssid[33], ip[16], server[128]; wifi_manager_get_status(ssid, sizeof(ssid), ip, sizeof(ip)); wifi_manager_get_server_url(server, sizeof(server)); return snprintf(b, n, "Wi-Fi: %s\nSSID: %s\nIP: %s\nServer: %s\n", wifi_manager_is_connected() ? "connected" : wifi_manager_is_configured() ? "configured; connecting" : "not configured", ssid[0] ? ssid : "-", ip[0] ? ip : "-", server[0] ? server : "-"); } if (!strcmp(v, "clear")) return finish(wifi_manager_clear_credentials(), b, n, "saved Wi-Fi cleared"); return fail(b, n, "wifi set is configured from the web app"); }
    if (!strcmp(a, "display")) { v = strtok_r(NULL, " \t\r\n", &save); if (!v) return fail(b, n, "display subcommand required"); if (!strcmp(v, "status")) return display_status(b, n); if (!strcmp(v, "next")) { display_next(); return snprintf(b, n, "OK display advanced\n"); } if (!strcmp(v, "output")) { v = strtok_r(NULL, " \t\r\n", &save); if (v && !strcasecmp(v, "sine1")) display_select(DISPLAY_SINE1); else if (v && !strcasecmp(v, "sine2")) display_select(DISPLAY_SINE2); else if (v && !strcasecmp(v, "square")) display_select(DISPLAY_SQUARE); else return fail(b, n, "display output must be sine1, sine2, or square"); return snprintf(b, n, "OK display output selected\n"); } if (!strcmp(v, "rotate")) { v = strtok_r(NULL, " \t\r\n", &save); if (!v || (strcasecmp(v, "on") && strcasecmp(v, "off"))) return fail(b, n, "rotate requires on or off"); display_set_rotation(!strcasecmp(v, "on")); return snprintf(b, n, "OK display rotation updated\n"); } if (!strcmp(v, "interval")) { v = strtok_r(NULL, " \t\r\n", &save); return finish(v ? display_set_interval((uint32_t)strtoul(v, NULL, 10)) : ESP_ERR_INVALID_ARG, b, n, "display interval updated"); } }
    return fail(b, n, "unknown command");
}

static const char *json_after(const char *j, const char *key) { const char *p = strstr(j, key); return p ? strchr(p, ':') + 1 : NULL; }
static bool json_string(const char *j, const char *key, char *out, size_t cap) { const char *p = json_after(j, key), *e; size_t len; if (!p) return false; while (isspace((unsigned char)*p)) ++p; if (*p++ != '"') return false; e = strchr(p, '"'); if (!e) return false; len = (size_t)(e - p); if (len >= cap) return false; memcpy(out, p, len); out[len] = 0; return true; }
static bool json_number(const char *j, const char *key, double *out) { const char *p = json_after(j, key); char *e; if (!p) return false; errno = 0; *out = strtod(p, &e); return e != p && !errno && isfinite(*out); }
static bool json_uint(const char *j, const char *key, uint32_t *out) { double x; if (!json_number(j, key, &x) || x < 0 || x > 4294967295.0 || floor(x) != x) return false; *out = (uint32_t)x; return true; }
static bool json_bool(const char *j, const char *key, bool *out) { const char *p = json_after(j, key); if (!p) return false; while (isspace((unsigned char)*p)) ++p; if (!strncmp(p, "true", 4)) *out = true; else if (!strncmp(p, "false", 5)) *out = false; else return false; return true; }

int command_api_execute_json(const char *j, char *b, size_t n)
{
    dds_state_t s; dds_output_t output; uint32_t reg = 0, hz; double number; bool enabled, connect = true; char value[128], ssid[33], password[65], server[128]; esp_err_t err = ESP_ERR_INVALID_ARG;
    if (!j || strlen(j) >= 512) return snprintf(b, n, "{\"ok\":false,\"error\":\"invalid command\"}");
    if (strstr(j, "\"get_state\"")) return command_api_get_state_json(b, n);
    dds_controller_get_state(&s); output = s.active_output;
    if (json_string(j, "\"output\"", value, sizeof(value)) && !parse_output_name(value, &output)) return snprintf(b, n, "{\"ok\":false,\"error\":\"output must be s1, s2, or s3\"}");
    if (strstr(j, "\"select_output\"")) err = dds_controller_select_output(output);
    else if (strstr(j, "\"set_frequency\"")) { if (json_uint(j, "\"register\"", &reg) && reg <= 1 && json_uint(j, "\"hz\"", &hz)) err = dds_controller_set_frequency_for_output(output, (uint8_t)reg, hz); else if (!strstr(j, "\"register\"" ) && json_uint(j, "\"hz\"", &hz)) err = dds_controller_set_frequency_for_output(output, 0, hz); }
    else if (strstr(j, "\"select_frequency\"")) { if (json_uint(j, "\"register\"", &reg) && reg <= 1) err = dds_controller_select_frequency_for_output(output, (uint8_t)reg); }
    else if (strstr(j, "\"set_phase\"")) { if (json_uint(j, "\"register\"", &reg) && reg <= 1 && json_number(j, "\"degrees\"", &number) && number >= 0 && number < 360) err = dds_controller_set_phase_for_output(output, (uint8_t)reg, (float)number); else if (!strstr(j, "\"register\"" ) && json_number(j, "\"degrees\"", &number) && number >= 0 && number < 360) err = dds_controller_set_phase_for_output(output, 0, (float)number); }
    else if (strstr(j, "\"select_phase\"")) { if (json_uint(j, "\"register\"", &reg) && reg <= 1) err = dds_controller_select_phase_for_output(output, (uint8_t)reg); }
    else if (strstr(j, "\"set_waveform\"")) { if (json_string(j, "\"waveform\"", value, sizeof(value))) err = !strcasecmp(value, "sine") ? dds_controller_set_waveform_for_output(output, DDS_WAVE_SINE) : !strcasecmp(value, "triangle") ? dds_controller_set_waveform_for_output(output, DDS_WAVE_TRIANGLE) : !strcasecmp(value, "square") ? dds_controller_set_waveform_for_output(output, DDS_WAVE_SQUARE) : ESP_ERR_INVALID_ARG; }
    else if (strstr(j, "\"set_output\"")) { if (json_bool(j, "\"enabled\"", &enabled)) err = dds_controller_set_output_for_output(output, enabled); }
    else if (strstr(j, "\"set_wifi\"")) {
        if (json_string(j, "\"ssid\"", ssid, sizeof(ssid)) &&
            json_string(j, "\"password\"", password, sizeof(password))) {
            (void)json_bool(j, "\"connect\"", &connect);
            if (json_string(j, "\"server_url\"", server, sizeof(server))) {
                err = wifi_manager_set_configuration(ssid, password, server, connect);
            } else {
                err = wifi_manager_set_credentials(ssid, password, connect);
            }
        }
    }
    else if (strstr(j, "\"clear_wifi\"")) err = wifi_manager_clear_credentials();
    else return snprintf(b, n, "{\"ok\":false,\"error\":\"unsupported command\"}");
    if (err != ESP_OK) return snprintf(b, n, "{\"ok\":false,\"error\":\"invalid value or hardware write failed\"}");
    changed(); return snprintf(b, n, "{\"ok\":true,\"message\":\"command applied\"}");
}
