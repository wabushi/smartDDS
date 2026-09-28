#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password, bool connect);
esp_err_t wifi_manager_set_configuration(const char *ssid, const char *password, const char *server_url, bool connect);
esp_err_t wifi_manager_clear_credentials(void);
bool wifi_manager_is_configured(void);
bool wifi_manager_is_connected(void);
void wifi_manager_get_status(char *ssid, size_t ssid_capacity, char *ip, size_t ip_capacity);
void wifi_manager_get_server_url(char *url, size_t capacity);
