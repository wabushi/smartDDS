#include "wifi_manager.h"
#include "config/board_config.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "WIFI";
static bool s_initialized, s_started, s_configured, s_connected;
static char s_ssid[33], s_password[64], s_ip[16], s_server_url[128];
static TaskHandle_t s_report_task;
static esp_timer_handle_t s_reconnect_timer;
static uint32_t s_reconnect_attempt;
static bool s_reconfigure_pending;

#define WIFI_REPORT_INTERVAL_MS 30000
#define WIFI_REPORT_FAILURE_INTERVAL_MS 300000
#define WIFI_REPORT_TASK_STACK 4096
#define WIFI_RECONNECT_MIN_MS 500U
#define WIFI_RECONNECT_MAX_MS 10000U

static const char *disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE: return "authentication expired";
    case WIFI_REASON_AUTH_LEAVE: return "authentication left";
    case WIFI_REASON_ASSOC_EXPIRE: return "association expired";
    case WIFI_REASON_ASSOC_TOOMANY: return "AP association limit";
    case WIFI_REASON_NOT_AUTHED: return "not authenticated";
    case WIFI_REASON_NOT_ASSOCED: return "not associated";
    case WIFI_REASON_ASSOC_LEAVE: return "association left";
    case WIFI_REASON_ASSOC_NOT_AUTHED: return "association not authenticated";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD: return "power capability mismatch";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD: return "channel mismatch";
    case WIFI_REASON_IE_INVALID: return "invalid information element";
    case WIFI_REASON_MIC_FAILURE: return "MIC failure";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4-way handshake timeout";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "group-key timeout";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS: return "4-way information mismatch";
    case WIFI_REASON_GROUP_CIPHER_INVALID: return "invalid group cipher";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID: return "invalid pairwise cipher";
    case WIFI_REASON_AKMP_INVALID: return "invalid authentication mode";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION: return "unsupported RSN version";
    case WIFI_REASON_INVALID_RSN_IE_CAP: return "invalid RSN capabilities";
    case WIFI_REASON_802_1X_AUTH_FAILED: return "802.1X authentication failed";
    case WIFI_REASON_CIPHER_SUITE_REJECTED: return "cipher suite rejected";
    case WIFI_REASON_BEACON_TIMEOUT: return "beacon timeout";
    case WIFI_REASON_NO_AP_FOUND: return "AP not found";
    case WIFI_REASON_AUTH_FAIL: return "authentication failed";
    case WIFI_REASON_ASSOC_FAIL: return "association failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "handshake timeout";
    case WIFI_REASON_CONNECTION_FAIL: return "connection failed";
    case WIFI_REASON_AP_TSF_RESET: return "AP timing reset";
    case WIFI_REASON_ROAMING: return "roaming";
    default: return "unclassified";
    }
}

static uint32_t reconnect_delay_ms(void)
{
    uint32_t delay = WIFI_RECONNECT_MIN_MS;
    uint32_t shifts = s_reconnect_attempt > 5U ? 5U : s_reconnect_attempt;
    delay <<= shifts;
    return delay > WIFI_RECONNECT_MAX_MS ? WIFI_RECONNECT_MAX_MS : delay;
}

static void reconnect_timer_callback(void *arg)
{
    (void)arg;
    if (!s_started || !s_configured || s_connected) return;
    ESP_LOGI(TAG, "Wi-Fi connection attempt %lu", (unsigned long)(s_reconnect_attempt + 1U));
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "Unable to start Wi-Fi connection: %s", esp_err_to_name(err));
    }
}

static void schedule_reconnect(uint32_t delay_ms)
{
    if (s_reconnect_timer == NULL || !s_started || !s_configured || s_connected) return;
    /* Callers use zero to mean "as soon as possible", but esp_timer does not
     * accept a zero one-shot timeout. The short delay also lets the Wi-Fi
     * event task finish processing a preceding disconnect. */
    if (delay_ms < 100U) delay_ms = 100U;
    esp_err_t err = esp_timer_stop(s_reconnect_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Unable to stop reconnect timer: %s", esp_err_to_name(err));
    }
    err = esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
    if (err != ESP_OK) ESP_LOGW(TAG, "Unable to schedule reconnect: %s", esp_err_to_name(err));
}

static bool report_connection(void)
{
    char url[192];
    char body[128];
    if (!s_connected || s_server_url[0] == '\0' || s_ip[0] == '\0') return false;

    const bool trailing_slash = s_server_url[strlen(s_server_url) - 1] == '/';
    int url_len = snprintf(url, sizeof(url), "%s%sapi/device/register",
                           s_server_url, trailing_slash ? "" : "/");
    int body_len = snprintf(body, sizeof(body),
                            "{\"device\":\"AD9834-DDS\",\"ip\":\"%s\",\"connected\":true}",
                            s_ip);
    if (url_len < 0 || (size_t)url_len >= sizeof(url) ||
        body_len < 0 || (size_t)body_len >= sizeof(body)) {
        ESP_LOGE(TAG, "Wi-Fi server registration payload is too long");
        return false;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Unable to create server registration client");
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, body_len);
    esp_err_t err = esp_http_client_perform(client);
    bool reported = false;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
            ESP_LOGI(TAG, "Connection reported to server (%s)", s_server_url);
            reported = true;
        } else {
            ESP_LOGW(TAG, "Server registration returned HTTP %d", status);
        }
    } else {
        ESP_LOGW(TAG, "Unable to report connection to server: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return reported;
}

static void connection_report_task(void *arg)
{
    (void)arg;
    uint32_t interval_ms = WIFI_REPORT_INTERVAL_MS;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(interval_ms));
        interval_ms = report_connection() ? WIFI_REPORT_INTERVAL_MS
                                          : WIFI_REPORT_FAILURE_INTERVAL_MS;
    }
}

static void disable_wifi_power_save(void)
{
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to disable Wi-Fi power save: %s", esp_err_to_name(err));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        disable_wifi_power_save();
        ESP_LOGI(TAG, "Wi-Fi station started with power save disabled");
        if (s_configured) esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        /* Re-apply after association. Some ESP-IDF/driver combinations reset
         * station power-save while negotiating with the AP. */
        disable_wifi_power_save();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)data;
        s_connected = false;
        s_ip[0] = '\0';
        if (!s_configured || !s_started) return;
        if (s_reconfigure_pending) {
            s_reconfigure_pending = false;
            s_reconnect_attempt = 0;
            ESP_LOGI(TAG, "Wi-Fi configuration changed; reconnecting");
            schedule_reconnect(100U);
            return;
        }
        uint32_t delay = reconnect_delay_ms();
        ESP_LOGW(TAG, "Wi-Fi disconnected: reason=%u (%s); retry in %lu ms",
                 event != NULL ? event->reason : 0U,
                 event != NULL ? disconnect_reason_name(event->reason) : "missing event data",
                 (unsigned long)delay);
        if (s_reconnect_attempt < UINT32_MAX) s_reconnect_attempt++;
        schedule_reconnect(delay);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        disable_wifi_power_save();
        s_reconnect_attempt = 0;
        if (s_reconnect_timer != NULL) esp_timer_stop(s_reconnect_timer);
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI(TAG, "Wi-Fi connected, IP=%s, RSSI=%d dBm, channel=%u",
                     s_ip, ap.rssi, ap.primary);
        } else {
            ESP_LOGI(TAG, "Wi-Fi connected, IP=%s", s_ip);
        }
        wifi_ps_type_t power_save;
        if (esp_wifi_get_ps(&power_save) == ESP_OK) {
            ESP_LOGI(TAG, "Wi-Fi power save mode=%d (%s)", power_save,
                     power_save == WIFI_PS_NONE ? "disabled" : "enabled");
        }
        if (s_report_task != NULL) xTaskNotifyGive(s_report_task);
    }
}

static esp_err_t apply_config_and_connect(void)
{
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, s_ssid, strlen(s_ssid));
    memcpy(config.sta.password, s_password, strlen(s_password));
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set station mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "set Wi-Fi config failed");
    if (!s_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi failed");
        s_started = true;
    } else {
        /* A successful disconnect emits WIFI_EVENT_STA_DISCONNECTED; the
         * event handler reconnects with the newly installed configuration.
         * If the station was already disconnected, connect it directly. */
        s_reconfigure_pending = true;
        esp_err_t err = esp_wifi_disconnect();
        if (err == ESP_ERR_WIFI_NOT_CONNECT) {
            s_reconfigure_pending = false;
            schedule_reconnect(100U);
        } else if (err != ESP_OK) {
            s_reconfigure_pending = false;
            ESP_LOGE(TAG, "Disconnect before Wi-Fi reconfiguration failed: %s",
                     esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t wifi_manager_init(void)
{
    if (s_initialized) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if (esp_netif_create_default_wifi_sta() == NULL) return ESP_ERR_NO_MEM;
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "Wi-Fi init failed");
    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = reconnect_timer_callback,
        .name = "wifi_reconnect",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&reconnect_timer_args, &s_reconnect_timer), TAG,
                        "Wi-Fi reconnect timer creation failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL), TAG, "Wi-Fi handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL), TAG, "IP handler failed");
    nvs_handle_t handle;
    err = nvs_open("wifi", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t ssid_size = sizeof(s_ssid), password_size = sizeof(s_password), url_size = sizeof(s_server_url);
        esp_err_t ssid_err = nvs_get_str(handle, "ssid", s_ssid, &ssid_size);
        esp_err_t password_err = nvs_get_str(handle, "password", s_password, &password_size);
        esp_err_t url_err = nvs_get_str(handle, "server_url", s_server_url, &url_size);
        if (url_err == ESP_ERR_NVS_NOT_FOUND) s_server_url[0] = '\0';
        if (s_server_url[0] == '\0') {
            snprintf(s_server_url, sizeof(s_server_url), "%s", WIFI_DEFAULT_SERVER_URL);
            ESP_LOGI(TAG, "Using default control server: %s", s_server_url);
        }
        if (ssid_err == ESP_OK && password_err == ESP_OK && s_ssid[0] != '\0') {
            size_t password_len = strlen(s_password);
            s_configured = password_len == 0 || (password_len >= 8 && password_len <= 63);
        }
        if (!s_configured) {
            s_ssid[0] = '\0';
            s_password[0] = '\0';
        }
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Unable to read saved Wi-Fi credentials: %s", esp_err_to_name(err));
    }
    s_initialized = true;
    if (xTaskCreate(connection_report_task, "wifi_report", WIFI_REPORT_TASK_STACK,
                    NULL, 4, &s_report_task) != pdPASS) {
        s_report_task = NULL;
        ESP_LOGE(TAG, "Unable to start Wi-Fi server reporting task");
        return ESP_ERR_NO_MEM;
    }
    if (s_configured) { ESP_LOGI(TAG, "Saved Wi-Fi credentials found; connecting"); return apply_config_and_connect(); }
    ESP_LOGI(TAG, "No saved Wi-Fi credentials; station mode is idle");
    return ESP_OK;
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password, bool connect)
{
    char server_url[sizeof(s_server_url)];
    snprintf(server_url, sizeof(server_url), "%s", s_server_url);
    return wifi_manager_set_configuration(ssid, password, server_url, connect);
}

esp_err_t wifi_manager_set_configuration(const char *ssid, const char *password, const char *server_url, bool connect)
{
    if (!s_initialized || ssid == NULL || password == NULL || server_url == NULL) return ESP_ERR_INVALID_STATE;
    size_t ssid_len = strlen(ssid), password_len = strlen(password);
    if (ssid_len == 0 || ssid_len > 32 || password_len > 63 || (password_len > 0 && password_len < 8) || strlen(server_url) >= sizeof(s_server_url)) return ESP_ERR_INVALID_ARG;
    const bool credentials_changed = strcmp(s_ssid, ssid) != 0 || strcmp(s_password, password) != 0;
    const bool server_changed = strcmp(s_server_url, server_url) != 0;
    if (!credentials_changed && !server_changed && s_configured) {
        if (!connect || s_connected) return ESP_OK;
        if (s_started) {
            schedule_reconnect(0U);
            return ESP_OK;
        }
        return apply_config_and_connect();
    }

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open("wifi", NVS_READWRITE, &handle), TAG, "open Wi-Fi storage failed");
    esp_err_t err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "password", password);
    if (err == ESP_OK) err = nvs_set_str(handle, "server_url", server_url);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle); ESP_RETURN_ON_ERROR(err, TAG, "save Wi-Fi credentials failed");
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_password, sizeof(s_password), "%s", password);
    snprintf(s_server_url, sizeof(s_server_url), "%s", server_url);
    s_configured = true;
    if (!connect) return ESP_OK;
    if (!credentials_changed && s_connected) {
        ESP_LOGI(TAG, "Control server updated without restarting Wi-Fi");
        if (s_report_task != NULL) xTaskNotifyGive(s_report_task);
        return ESP_OK;
    }
    return apply_config_and_connect();
}

esp_err_t wifi_manager_clear_credentials(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open("wifi", NVS_READWRITE, &handle), TAG, "open Wi-Fi storage failed");
    esp_err_t err = nvs_erase_key(handle, "ssid");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, "password");
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_erase_key(handle, "server_url");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, TAG, "clear Wi-Fi credentials failed");
    /* Clear the configured flag before disconnecting so the disconnect event
     * cannot immediately reconnect using credentials that were just erased. */
    s_configured = false;
    s_reconfigure_pending = false;
    s_reconnect_attempt = 0;
    if (s_reconnect_timer != NULL) esp_timer_stop(s_reconnect_timer);
    if (s_started) { esp_wifi_disconnect(); esp_wifi_stop(); s_started = false; }
    s_connected = false; s_ssid[0] = '\0'; s_password[0] = '\0'; s_server_url[0] = '\0'; s_ip[0] = '\0'; return ESP_OK;
}
void wifi_manager_get_server_url(char *url, size_t capacity)
{
    if (url != NULL && capacity > 0) snprintf(url, capacity, "%s", s_server_url);
}
bool wifi_manager_is_configured(void) { return s_configured; }
bool wifi_manager_is_connected(void) { return s_connected; }
void wifi_manager_get_status(char *ssid, size_t ssid_capacity, char *ip, size_t ip_capacity)
{
    if (ssid != NULL && ssid_capacity > 0) snprintf(ssid, ssid_capacity, "%s", s_ssid);
    if (ip != NULL && ip_capacity > 0) snprintf(ip, ip_capacity, "%s", s_ip);
}
