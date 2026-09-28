#include "display_hw.h"
#include "config/board_config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "DISPLAY";
#define OLED_WIDTH 72
#define OLED_HEIGHT 40
#define OLED_PAGES (OLED_HEIGHT / 8)
#define OLED_X_OFFSET 28
#define WAVEFORM_X 28
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static SemaphoreHandle_t s_render_mutex;

static esp_err_t write_cmds(const uint8_t *cmds, size_t count)
{
    uint8_t packet[17];
    while (count) {
        size_t n = count > 16 ? 16 : count;
        packet[0] = 0x00;
        memcpy(&packet[1], cmds, n);
        esp_err_t err = i2c_master_transmit(s_dev, packet, n + 1, 100);
        if (err != ESP_OK) return err;
        cmds += n;
        count -= n;
    }
    return ESP_OK;
}

static esp_err_t write_data(const uint8_t *data, size_t count)
{
    uint8_t packet[17];
    while (count) {
        size_t n = count > 16 ? 16 : count;
        packet[0] = 0x40;
        memcpy(&packet[1], data, n);
        esp_err_t err = i2c_master_transmit(s_dev, packet, n + 1, 100);
        if (err != ESP_OK) return err;
        data += n;
        count -= n;
    }
    return ESP_OK;
}

esp_err_t display_hw_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_DISPLAY_SDA_GPIO,
        .scl_io_num = BOARD_DISPLAY_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) return err;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_DISPLAY_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) return err;

    err = i2c_master_probe(s_bus, BOARD_DISPLAY_I2C_ADDR, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OLED probe failed at 0x%02X: %s", BOARD_DISPLAY_I2C_ADDR, esp_err_to_name(err));
        return err;
    }

    s_render_mutex = xSemaphoreCreateMutex();
    if (!s_render_mutex) return ESP_ERR_NO_MEM;

    static const uint8_t init[] = {
        /* SSD1306 ER 72x40 sequence, matching the controller's 128-column
         * memory layout and internal charge-pump configuration. */
        0xAE, 0xD5, 0x80, 0xA8, 0x27, 0xD3, 0x00, 0xAD, 0x30,
        0x8D, 0x14, 0x40, 0xA6, 0xA4, 0x20, 0x00, 0xA1, 0xC8,
        0xDA, 0x12, 0x81, 0xAF, 0xD9, 0x22, 0xDB, 0x20, 0x2E, 0xAF
    };
    err = write_cmds(init, sizeof(init));
    if (err == ESP_OK) {
        s_present = true;
        ESP_LOGI(TAG, "OLED detected: SSD1306-compatible 72x40 at 0x%02X (SDA GPIO%d, SCL GPIO%d)",
                 BOARD_DISPLAY_I2C_ADDR, BOARD_DISPLAY_SDA_GPIO, BOARD_DISPLAY_SCL_GPIO);
    }
    return err;
}

bool display_hw_present(void) { return s_present; }

static void glyph(char c, uint8_t out[5])
{
    memset(out, 0, 5);
    if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
    /* Compact 5x7 glyphs for the normal UI and startup test. */
    static const uint8_t a[5] = {0x7E,0x11,0x11,0x11,0x7E};
    static const uint8_t e[5] = {0x7F,0x49,0x49,0x49,0x41};
    static const uint8_t i[5] = {0x00,0x41,0x7F,0x41,0x00};
    static const uint8_t n[5] = {0x7F,0x04,0x08,0x10,0x7F};
    static const uint8_t o[5] = {0x3E,0x41,0x41,0x41,0x3E};
    static const uint8_t q[5] = {0x3E,0x41,0x51,0x21,0x5E};
    static const uint8_t r[5] = {0x7F,0x09,0x19,0x29,0x46};
    static const uint8_t s[5] = {0x46,0x49,0x49,0x49,0x31};
    static const uint8_t t[5] = {0x01,0x01,0x7F,0x01,0x01};
    static const uint8_t u[5] = {0x3F,0x40,0x40,0x40,0x3F};
    static const uint8_t z[5] = {0x61,0x51,0x49,0x45,0x43};
    static const uint8_t f[5] = {0x7F,0x09,0x09,0x01,0x01};
    static const uint8_t g[5] = {0x3E,0x41,0x49,0x49,0x7A};
    static const uint8_t h[5] = {0x7F,0x08,0x08,0x08,0x7F};
    static const uint8_t k[5] = {0x7F,0x08,0x14,0x22,0x41};
    static const uint8_t l[5] = {0x7F,0x40,0x40,0x40,0x40};
    static const uint8_t m[5] = {0x7F,0x02,0x0C,0x02,0x7F};
    static const uint8_t three[5] = {0x21,0x41,0x45,0x4B,0x31};
    static const uint8_t four[5] = {0x0C,0x14,0x24,0x7F,0x04};
    /* Standard 5x7 digit 5.  The previous bitmap used 0x79/0x4F at
     * the ends, which added pixels and made a displayed 5 resemble a 2. */
    static const uint8_t five[5] = {0x4F,0x49,0x49,0x49,0x31};
    static const uint8_t six[5] = {0x3E,0x49,0x49,0x49,0x30};
    static const uint8_t seven[5] = {0x01,0x71,0x09,0x05,0x03};
    static const uint8_t eight[5] = {0x36,0x49,0x49,0x49,0x36};
    static const uint8_t nine[5] = {0x06,0x49,0x49,0x29,0x1E};
    static const uint8_t dot[5] = {0x00,0x60,0x60,0x00,0x00};
    static const uint8_t percent[5] = {0x63,0x13,0x08,0x64,0x63};
    static const uint8_t one[5] = {0x00,0x42,0x7F,0x40,0x00};
    static const uint8_t two[5] = {0x62,0x51,0x49,0x49,0x46};
    static const uint8_t zero[5] = {0x3E,0x51,0x49,0x45,0x3E};
    const uint8_t *selected = NULL;
    switch (c) {
        case 'A': selected = a; break;
        case 'E': selected = e; break;
        case 'I': selected = i; break;
        case 'N': selected = n; break;
        case 'O': selected = o; break;
        case 'Q': selected = q; break;
        case 'R': selected = r; break;
        case 'S': selected = s; break;
        case 'T': selected = t; break;
        case 'U': selected = u; break;
        case 'Z': selected = z; break;
        case 'F': selected = f; break;
        case 'G': selected = g; break;
        case 'H': selected = h; break;
        case 'K': selected = k; break;
        case 'L': selected = l; break;
        case 'M': selected = m; break;
        case '0': selected = zero; break;
        case '1': selected = one; break;
        case '2': selected = two; break;
        case '3': selected = three; break;
        case '4': selected = four; break;
        case '5': selected = five; break;
        case '6': selected = six; break;
        case '7': selected = seven; break;
        case '8': selected = eight; break;
        case '9': selected = nine; break;
        case '.': selected = dot; break;
        case '%': selected = percent; break;
        default: break;
    }
    if (selected) memcpy(out, selected, 5);
}

static esp_err_t set_page_and_column_at(uint8_t page, uint8_t column)
{
    const uint8_t cmds[] = {
        (uint8_t)(0xB0 | page),
        (uint8_t)(0x00 | (column & 0x0F)),
        (uint8_t)(0x10 | (column >> 4)),
    };
    return write_cmds(cmds, sizeof(cmds));
}

static esp_err_t set_page_and_column(uint8_t page)
{
    return set_page_and_column_at(page, OLED_X_OFFSET);
}

esp_err_t display_hw_fill(bool on)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_render_mutex, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    uint8_t row[OLED_WIDTH];
    memset(row, on ? 0xFF : 0x00, sizeof(row));
    for (uint8_t page = 0; page < OLED_PAGES; ++page) {
        esp_err_t err = set_page_and_column(page);
        if (err != ESP_OK) { result = err; break; }
        err = write_data(row, sizeof(row));
        if (err != ESP_OK) { result = err; break; }
    }
    xSemaphoreGive(s_render_mutex);
    return result;
}

esp_err_t display_hw_show_text(const char *line1, const char *line2, const char *line3)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_render_mutex, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    for (int line = 0; line < OLED_PAGES; ++line) {
        esp_err_t err = set_page_and_column((uint8_t)line);
        if (err != ESP_OK) { result = err; break; }
        const char *text = line == 0 ? line1 : line == 1 ? line2 : line3;
        uint8_t row[OLED_WIDTH] = {0};
        int x = 0;
        for (size_t i = 0; text && text[i] && x + 5 <= OLED_WIDTH; ++i) {
            uint8_t g[5]; glyph(text[i], g);
            memcpy(&row[x], g, 5); x += 6;
        }
        err = write_data(row, sizeof(row));
        if (err != ESP_OK) { result = err; break; }
    }
    xSemaphoreGive(s_render_mutex);
    return result;
}

esp_err_t display_hw_show_bitmap(const uint8_t *bitmap)
{
    if (!s_present || !bitmap) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_render_mutex, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    for (uint8_t page = 0; page < OLED_PAGES; ++page) {
        esp_err_t err = set_page_and_column(page);
        if (err != ESP_OK) { result = err; break; }
        err = write_data(&bitmap[(size_t)page * OLED_WIDTH], OLED_WIDTH);
        if (err != ESP_OK) { result = err; break; }
    }
    xSemaphoreGive(s_render_mutex);
    return result;
}

static void set_pixel(uint8_t rows[OLED_PAGES][OLED_WIDTH], int x, int y)
{
    if (x >= 0 && x < OLED_WIDTH && y >= 0 && y < 24) {
        rows[2 + y / 8][x] |= (uint8_t)(1U << (y % 8));
    }
}

static void draw_segment(uint8_t bitmap[22][42], int x0, int y0, int x1, int y1)
{
    /* Integer line rasterization keeps the triangle fully symmetric and
     * makes the bitmap deterministic on the 1-bit OLED. */
    int dx = x1 - x0;
    int sx = dx >= 0 ? 1 : -1;
    dx = dx >= 0 ? dx : -dx;
    int dy = y1 - y0;
    int sy = dy >= 0 ? 1 : -1;
    dy = dy >= 0 ? dy : -dy;
    int err = (dx > dy ? dx : -dy) / 2;
    for (;;) {
        if (x0 >= 0 && x0 < 42 && y0 >= 0 && y0 < 22) bitmap[y0][x0] = 1;
        if (x0 == x1 && y0 == y1) break;
        int e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 < dy) { err += dx; y0 += sy; }
    }
}

static void build_waveform_bitmap(uint8_t bitmap[22][42], display_waveform_t waveform,
                                  int *x0_out, int *x1_out)
{
    /* The bottom three pages provide a symbolic, non-measured waveform.
     * The sine graphic is a literal transcription of the supplied 42x22
     * reference bitmap. Its five-pixel margins are intentional. */
    int x0 = 5;
    int x1 = 36;
    memset(bitmap, 0, 22 * 42);
    if (waveform == DISPLAY_WAVE_SQUARE) {
        /* Build the square symbol in the reference bitmap coordinates. */
        for (int x = x0; x <= x1; ++x) {
            int y = (x < 16 || x >= 27) ? 18 : 5;
            bitmap[y][x] = 1;
            bitmap[y + 1][x] = 1;
        }
        for (int x = x0; x <= x1; ++x) {
            if (x == 16 || x == 27) {
                /* End exactly on the two-pixel bottom rail; do not leave
                 * isolated pixels below the waveform. */
                for (int y = 5; y <= 19; ++y) {
                    bitmap[y][x] = 1;
                }
            }
        }
    } else if (waveform == DISPLAY_WAVE_TRIANGLE) {
        /* The supplied 44x20 reference has two complete periods, with the
         * trace bounded by y=3..16.  Use the full 42-column animation box
         * for this waveform so no edge columns are discarded during the
         * cyclic shift.  Draw one half and mirror it about x=20.5; this
         * gives a deterministic, pixel-symmetric bitmap on the OLED. */
        x0 = 0;
        x1 = 41;
        draw_segment(bitmap, 0, 3, 10, 16);
        draw_segment(bitmap, 10, 16, 20, 3);
        for (int y = 0; y < 22; ++y) {
            for (int x = 0; x < 21; ++x) {
                if (bitmap[y][x]) bitmap[y][41 - x] = 1;
            }
        }
    } else {
        /* Symmetric reconstruction of the supplied 42x22 bitmap.
         * The bitmap canvas is centered on the OLED; the shape itself is
         * mirrored about canvas x=20.5 (every left/right pair sums to 41).
         * This removes the one-pixel visual bias in the source bitmap while
         * preserving its proportions, margins, and rounded profile. */
        static const struct { uint8_t y, x0, x1; } runs[] = {
            { 5, 18, 23 },
            { 6, 17, 17 }, { 6, 24, 24 },
            { 7, 16, 16 }, { 7, 25, 25 },
            { 8, 15, 15 }, { 8, 26, 26 },
            { 9, 14, 14 }, { 9, 27, 27 },
            {10, 14, 14 }, {10, 27, 27 },
            {11, 14, 14 }, {11, 27, 27 },
            {12, 13, 13 }, {12, 28, 28 },
            {13, 13, 13 }, {13, 28, 28 },
            {14, 13, 13 }, {14, 28, 28 },
            {15, 12, 12 }, {15, 29, 29 },
            {16, 11, 11 }, {16, 30, 30 },
            {17, 10, 10 }, {17, 31, 31 },
            {18,  5,  9 }, {18, 32, 36 },
        };
        for (size_t i = 0; i < sizeof(runs) / sizeof(runs[0]); ++i) {
            for (int x = runs[i].x0; x <= runs[i].x1; ++x) {
                bitmap[runs[i].y][x] = 1;
            }
        }
    }

    *x0_out = x0;
    *x1_out = x1;
}

static void draw_waveform(uint8_t rows[OLED_PAGES][OLED_WIDTH], display_waveform_t waveform,
                          uint32_t waveform_offset)
{
    const int bitmap_x = WAVEFORM_X;
    uint8_t bitmap[22][42];
    int x0, x1;
    build_waveform_bitmap(bitmap, waveform, &x0, &x1);

    /* Shift only the waveform bounding box. Each step removes its first
     * column and appends that column at the right edge, preserving a fixed
     * graphic area rather than moving the whole display. */
    const int width = x1 - x0 + 1;
    const int shift = waveform_offset % width;
    for (int y = 0; y < 22; ++y) {
        for (int src = x0; src <= x1; ++src) {
            if (bitmap[y][src]) {
                int relative = src - x0;
                int dst = (relative + width - shift) % width;
                set_pixel(rows, bitmap_x + x0 + dst, y);
            }
        }
    }
}

esp_err_t display_hw_animate_output(display_waveform_t waveform, uint32_t waveform_offset)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;

    const int bitmap_x = WAVEFORM_X;
    uint8_t bitmap[22][42];
    int x0, x1;
    build_waveform_bitmap(bitmap, waveform, &x0, &x1);
    const int width = x1 - x0 + 1;
    const int shift = waveform_offset % width;
    uint8_t region[3][42] = {{0}};

    /* Build only the three waveform pages. The complete region is written
     * each frame, so old pixels are cleared without a full-screen refresh. */
    for (int y = 0; y < 22; ++y) {
        for (int src = x0; src <= x1; ++src) {
            if (bitmap[y][src]) {
                int relative = src - x0;
                int dst = (relative + width - shift) % width;
                /* region[] is relative to the waveform window; screen
                 * coordinates are applied only when positioning the OLED
                 * column below. */
                region[y / 8][x0 + dst] |= (uint8_t)(1U << (y % 8));
            }
        }
    }

    xSemaphoreTake(s_render_mutex, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    /* Always rewrite the complete 42-column waveform window.  Sine and
     * square use an inset 32-column shape while triangle uses the full
     * window; writing all 42 columns prevents old triangle pixels from
     * surviving outside the narrower shape. */
    const uint8_t column = (uint8_t)(OLED_X_OFFSET + bitmap_x);
    for (uint8_t page = 0; page < 3; ++page) {
        esp_err_t err = set_page_and_column_at((uint8_t)(2 + page), column);
        if (err != ESP_OK) { result = err; break; }
        err = write_data(region[page], sizeof(region[page]));
        if (err != ESP_OK) { result = err; break; }
    }
    xSemaphoreGive(s_render_mutex);
    return result;
}

static void draw_text(uint8_t *row, int x, const char *text)
{
    for (size_t i = 0; text && text[i] && x + 5 <= OLED_WIDTH; ++i) {
        uint8_t g[5];
        glyph(text[i], g);
        memcpy(&row[x], g, sizeof(g));
        x += 6;
    }
}

esp_err_t display_hw_show_output(const char *output_id, const char *frequency,
                                 const char *output_state, const char *phase,
                                 const char *waveform_name, display_waveform_t waveform,
                                 uint32_t waveform_offset)
{
    if (!s_present) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_render_mutex, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    uint8_t rows[OLED_PAGES][OLED_WIDTH] = {{0}};

    /* Fixed two-column layout for the 72x40 display:
     * S#       | XX.XXMHZ
     * phase    | waveform
     * ON/OFF   | graphic
     */
    draw_text(rows[0], 0, output_id);
    draw_text(rows[0], 25, frequency);
    draw_text(rows[1], 0, phase);
    draw_text(rows[1], 28, waveform_name);
    draw_text(rows[3], 0, output_state);
    draw_waveform(rows, waveform, waveform_offset);

    for (int page = 0; page < 5; ++page) {
        esp_err_t err = set_page_and_column((uint8_t)page);
        if (err != ESP_OK) { result = err; break; }
        uint8_t row[OLED_WIDTH] = {0};
        memcpy(row, rows[page], OLED_WIDTH);
        err = write_data(row, sizeof(row));
        if (err != ESP_OK) { result = err; break; }
    }
    xSemaphoreGive(s_render_mutex);
    return result;
}
