#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "dds/dds_controller.h"
#include "display/display.h"
#include "transport/command_api.h"
#include "transport/serial_cli.h"
#include "transport/ble_service.h"
#include "transport/wifi_service.h"
#include "network/wifi_manager.h"

static const char *TAG="APP";
static void app_state_changed(void)
{
    ESP_LOGI(TAG, "DDS state changed; refreshing display and BLE state");
    display_notify_state_change();
    ble_service_notify_state();
}
void app_main(void)
{
    ESP_LOGI(TAG, "Reset reason: %d", (int)esp_reset_reason());
    esp_err_t e=nvs_flash_init();if(e==ESP_ERR_NVS_NO_FREE_PAGES||e==ESP_ERR_NVS_NEW_VERSION_FOUND){nvs_flash_erase();e=nvs_flash_init();}if(e!=ESP_OK)ESP_LOGE(TAG,"NVS init failed: %s",esp_err_to_name(e));
    e=wifi_manager_init();if(e!=ESP_OK)ESP_LOGE(TAG,"Wi-Fi init failed: %s",esp_err_to_name(e));
    e=dds_controller_init();if(e!=ESP_OK)ESP_LOGE(TAG,"DDS init failed: %s",esp_err_to_name(e));else ESP_LOGI(TAG,"AD9834 initialized; startup state is 1 MHz sine, phase 0 deg, output ON");
    e=display_init();if(e!=ESP_OK)ESP_LOGE(TAG,"display task failed: %s",esp_err_to_name(e));
    command_api_set_notify(app_state_changed);
    e=wifi_service_start();if(e!=ESP_OK)ESP_LOGE(TAG,"Wi-Fi command service failed: %s",esp_err_to_name(e));
    e=ble_service_start();if(e!=ESP_OK)ESP_LOGE(TAG,"BLE init failed: %s",esp_err_to_name(e));
    e=serial_cli_start();if(e!=ESP_OK)ESP_LOGE(TAG,"CLI failed: %s",esp_err_to_name(e));
    ESP_LOGI(TAG,"Ready. Use serial CLI: help");
}
