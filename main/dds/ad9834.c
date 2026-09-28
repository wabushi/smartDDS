#include "ad9834.h"
#include "config/board_config.h"
#include "config/dds_config.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include <math.h>

static const char *TAG = "AD9834";
static spi_device_handle_t s_spi;
static uint32_t s_mclk;
static uint16_t s_control;

#define CTRL_B28      0x2000
#define CTRL_RESET    0x0100
#define CTRL_SLEEP12  0x0040
#define CTRL_OPBITEN  0x0020
#define CTRL_SIGNPIB  0x0010
#define CTRL_DIV2     0x0008
#define CTRL_FSELECT  0x0800
#define CTRL_PSELECT  0x0400
#define CTRL_MODE     0x0002
#define REG_FREQ0     0x4000
#define REG_FREQ1     0x8000
#define REG_PHASE0    0xC000
#define REG_PHASE1    0xE000

static esp_err_t write_word(uint16_t word)
{
    // Use the transaction's inline TX storage explicitly. Without
    // SPI_TRANS_USE_TXDATA, tx_data is interpreted as a pointer by the SPI
    // driver, which is invalid on ESP-IDF and causes a load fault.
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 16,
        .tx_data = { (uint8_t)(word >> 8), (uint8_t)word },
    };
    return spi_device_transmit(s_spi, &t);
}

uint32_t ad9834_frequency_word(uint32_t hz, uint32_t mclk_hz)
{
    uint64_t numerator = ((uint64_t)hz << AD9834_FREQ_BITS) + (mclk_hz / 2U);
    return (uint32_t)(numerator / mclk_hz);
}

esp_err_t ad9834_init(uint32_t mclk_hz)
{
    s_mclk = mclk_hz;
    gpio_config_t rst = { .pin_bit_mask = 1ULL << DDS_RESET_GPIO, .mode = GPIO_MODE_OUTPUT,
                          .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                          .intr_type = GPIO_INTR_DISABLE };
    ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "reset GPIO");
    gpio_set_level(DDS_RESET_GPIO, 1);

    spi_bus_config_t bus = { .mosi_io_num = DDS_SDATA_GPIO, .miso_io_num = -1,
                             .sclk_io_num = DDS_SCLK_GPIO, .quadwp_io_num = -1, .quadhd_io_num = -1,
                             .max_transfer_sz = 2 };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED), TAG, "SPI bus");
    spi_device_interface_config_t dev = { .clock_speed_hz = AD9834_SPI_HZ, .mode = 2,
                                          .spics_io_num = DDS_FSYNC_GPIO, .queue_size = 1 };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev, &s_spi), TAG, "SPI device");
    s_control = CTRL_B28 | CTRL_RESET | CTRL_SLEEP12;
    return write_word(s_control);
}

esp_err_t ad9834_set_mclk(uint32_t mclk_hz)
{
    if (mclk_hz == 0 || mclk_hz > 100000000U) return ESP_ERR_INVALID_ARG;
    s_mclk = mclk_hz;
    return ESP_OK;
}

esp_err_t ad9834_reset(void)
{
    gpio_set_level(DDS_RESET_GPIO, 1);
    esp_rom_delay_us(2);
    s_control |= CTRL_RESET | CTRL_SLEEP12;
    esp_err_t err = write_word(s_control);
    gpio_set_level(DDS_RESET_GPIO, 0);
    return err;
}

esp_err_t ad9834_set_frequency(uint8_t reg, uint32_t hz)
{
    if (reg > 1 || hz > AD9834_MAX_OUTPUT_HZ) return ESP_ERR_INVALID_ARG;
    uint32_t word = ad9834_frequency_word(hz, s_mclk);
    uint16_t prefix = reg ? REG_FREQ1 : REG_FREQ0;
    ESP_RETURN_ON_ERROR(write_word(s_control | CTRL_B28), TAG, "frequency control");
    ESP_RETURN_ON_ERROR(write_word(prefix | (word & 0x3FFF)), TAG, "frequency low");
    return write_word(prefix | ((word >> 14) & 0x3FFF));
}

esp_err_t ad9834_set_phase(uint8_t reg, float degrees)
{
    if (reg > 1 || !isfinite(degrees) || degrees < 0.0f || degrees >= 360.0f) return ESP_ERR_INVALID_ARG;
    uint16_t value = (uint16_t)lroundf(degrees * 4096.0f / 360.0f) & 0x0FFF;
    return write_word((reg ? REG_PHASE1 : REG_PHASE0) | value);
}

esp_err_t ad9834_select_frequency(uint8_t reg)
{
    if (reg > 1) return ESP_ERR_INVALID_ARG;
    s_control = (s_control & ~CTRL_FSELECT) | (reg ? CTRL_FSELECT : 0);
    return write_word(s_control);
}

esp_err_t ad9834_select_phase(uint8_t reg)
{
    if (reg > 1) return ESP_ERR_INVALID_ARG;
    s_control = (s_control & ~CTRL_PSELECT) | (reg ? CTRL_PSELECT : 0);
    return write_word(s_control);
}

esp_err_t ad9834_set_waveform(ad9834_waveform_t waveform)
{
    s_control &= ~(CTRL_OPBITEN | CTRL_SIGNPIB | CTRL_DIV2 | CTRL_MODE);
    if (waveform == AD9834_WAVE_TRIANGLE) s_control |= CTRL_MODE;
    else if (waveform == AD9834_WAVE_SQUARE) s_control |= CTRL_OPBITEN | CTRL_SIGNPIB;
    else if (waveform != AD9834_WAVE_SINE) return ESP_ERR_INVALID_ARG;
    return write_word(s_control);
}

esp_err_t ad9834_set_output(bool enabled)
{
    if (enabled) {
        gpio_set_level(DDS_RESET_GPIO, 0);
        s_control &= ~(CTRL_RESET | CTRL_SLEEP12);
    } else {
        s_control |= CTRL_RESET | CTRL_SLEEP12;
        gpio_set_level(DDS_RESET_GPIO, 1);
    }
    return write_word(s_control);
}
