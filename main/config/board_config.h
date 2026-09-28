#pragma once

/* User-confirmed wiring. FS and PS on the DDS module are intentionally unused. */
#define DDS_FSYNC_GPIO  7
#define DDS_SCLK_GPIO   8
#define DDS_SDATA_GPIO  9
#define DDS_RESET_GPIO 10

/* Common ESP32-C3 SuperMini OLED variant: 0.42-inch 72x40 SSD1306-compatible OLED. */
#define BOARD_DISPLAY_PRESENT 1
#define BOARD_DISPLAY_SDA_GPIO 5
#define BOARD_DISPLAY_SCL_GPIO 6
#define BOARD_DISPLAY_I2C_ADDR 0x3C
#define BLE_DEVICE_NAME "AD9834-DDS"

/* HTTP endpoint used by the ESP32 to announce its Wi-Fi address to the local
 * control server. A URL supplied during BLE provisioning replaces this value.
 * Keeping a default also migrates credentials saved by older firmware, which
 * did not store a server URL. */
#define WIFI_DEFAULT_SERVER_URL "https://smartdds.erezh.com"
