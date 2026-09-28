idf_component_register(
    SRCS
        "app_main.c"
        "config/board_config.h"
        "config/dds_config.h"
        "dds/ad9834.c" "dds/dds_state.c" "dds/dds_controller.c"
        "measurement/measurement.c"
        "network/wifi_manager.c"
        "transport/command_api.c" "transport/serial_cli.c" "transport/ble_service.c" "transport/wifi_service.c"
        "display/display.c" "display/display_ui.c" "display/display_hw.c"
        "display/splash_bitmap.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash bt esp_wifi esp_event esp_netif esp_http_server esp_http_client
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror=return-type)
