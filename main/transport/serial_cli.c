#include "serial_cli.h"
#include "command_api.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
static void cli(void *arg)
{
    (void)arg;
    char line[160];
    char out[1200];
    size_t line_len = 0;
    uint8_t byte;

    printf("dds> ");
    fflush(stdout);
    while (true) {
        int received = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(100));
        if (received <= 0) continue;
        if (byte == '\r' || byte == '\n') {
            if (line_len > 0) {
                line[line_len] = '\0';
                command_api_execute_line(line, out, sizeof(out));
                fputs(out, stdout);
                line_len = 0;
            }
            printf("dds> ");
            fflush(stdout);
        } else if ((byte == '\b' || byte == 0x7f) && line_len > 0) {
            line_len--;
            fputs("\b \b", stdout);
            fflush(stdout);
        } else if (byte >= 0x20 && byte <= 0x7e && line_len < sizeof(line) - 1) {
            line[line_len++] = (char)byte;
            putchar((int)byte);
            fflush(stdout);
        }
    }
}
esp_err_t serial_cli_start(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        esp_err_t err = usb_serial_jtag_driver_install(&config);
        if (err != ESP_OK) {
            return err;
        }
    }
    return xTaskCreate(cli, "serial_cli", 8192, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
