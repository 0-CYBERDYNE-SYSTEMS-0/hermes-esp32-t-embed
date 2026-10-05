/* Compact on-device waterfall for the standard LILYGO T-Embed. */
#include "t_embed_ui.h"
#include "t_embed_gesture.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_W 320u
#define LCD_H 170u
#define LCD_ROW_BYTES (LCD_W * 2u)
#define LCD_SPI_HZ 40000000

#define PIN_LCD_SCLK GPIO_NUM_12
#define PIN_LCD_MOSI GPIO_NUM_11
#define PIN_LCD_CS GPIO_NUM_10
#define PIN_LCD_DC GPIO_NUM_13
#define PIN_LCD_RST GPIO_NUM_9
#define PIN_LCD_BL GPIO_NUM_15
#define PIN_POWER_ON GPIO_NUM_46
#define PIN_LED_DATA GPIO_NUM_42
#define PIN_LED_CLOCK GPIO_NUM_45
#define PIN_ENCODER_A GPIO_NUM_2
#define PIN_ENCODER_B GPIO_NUM_1
#define PIN_ENCODER_KEY GPIO_NUM_0
#define LED_COUNT 7u
#define LED_DIM_BRIGHTNESS 4u

#define HEADER_H 24u
#define GRAPH_X 0u
#define GRAPH_Y 24u
#define GRAPH_H 29u
#define AXIS_Y 53u
#define WATER_Y 61u
#define WATER_H 97u
#define FOOTER_Y 158u
#define FOOTER_H 12u
#define STRIP_ROWS 8u
#define CAPTURE_GAP_US 12000
#define RGB565_COLOR(r, g, b) \
    (uint16_t)((((r) & 0xf8u) << 8) | (((g) & 0xfcu) << 3) | ((b) >> 3))

typedef struct {
    uint8_t level;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} waterfall_color_stop_t;

static spi_device_handle_t lcd;
static uint8_t *waterfall; /* PSRAM history; transferred through internal DMA strips */
static uint8_t *dma_strip;
#define line dma_strip /* single scanline work area lives in the DMA strip */
#define trace_y (dma_strip + LCD_ROW_BYTES) /* second strip row remains available */
static unsigned water_head;
static unsigned step_mhz = 1;
static unsigned rate_code = 1;
static bool paused;
static int64_t next_capture_us;
static int64_t last_frame_us;
static pcnt_unit_handle_t encoder_pcnt;
static pcnt_channel_handle_t encoder_channel_a;
static pcnt_channel_handle_t encoder_channel_b;
static bool encoder_pcnt_ready;
static bool mic_leds_ready;
static bool mic_led_state_known;
static bool mic_led_enabled;
static bool mic_led_muted;
static int encoder_edges;
static t_embed_gesture_state_t gesture;
static bool gesture_initialized;
static bool sdr_menu_active;
static bool sdr_span_menu_active;
static bool sdr_menu_wait_release;
static unsigned sdr_menu_selection;
static unsigned sdr_span_selection;
static bool microphone_menu_active;
static bool microphone_menu_wait_release;
static unsigned microphone_menu_selection;
static bool home_requested;
static bool frame_flush_suppressed;
static bool overlay_cache_valid;
static const char *overlay_title;
static const char *const *overlay_items;
static const char *overlay_hint;
static unsigned overlay_count;
static unsigned overlay_selection;
static bool prompt_overlay_valid;
static uint32_t prompt_overlay_hash;
static unsigned prompt_overlay_scroll;
static unsigned prompt_overlay_selection;
static bool prompt_overlay_answer_view;
static unsigned prompt_overlay_outcome;
static bool mic_screen_drawn;
static bool mic_bridge_connected;
static bool mic_screen_muted;
static bool view_valid;
static t_embed_ui_view_t last_view;
static t_embed_ui_stats_t stats;

/* Tokyo Night colors keep the chrome dim and reserve brighter hues for signal. */
static const uint16_t COLOR_BG = RGB565_COLOR(13, 16, 30);
static const uint16_t COLOR_HEADER = RGB565_COLOR(26, 27, 38);
static const uint16_t COLOR_GRID = RGB565_COLOR(57, 66, 97);
static const uint16_t COLOR_TEXT = RGB565_COLOR(182, 194, 245);
static const uint16_t COLOR_DIM = RGB565_COLOR(107, 120, 163);
static const uint16_t COLOR_ACCENT = RGB565_COLOR(125, 207, 255);
static const uint16_t COLOR_PEAK = RGB565_COLOR(255, 158, 100);
static const uint16_t COLOR_MUTE = RGB565_COLOR(190, 78, 98);
static const waterfall_color_stop_t WATERFALL_COLORS[] = {
    { 0,   13,  16,  30 },
    { 64,  39,  52,  94 },
    { 128, 106, 82,  170 },
    { 192, 58,  153, 186 },
    { 255, 238, 151, 98 },
};

static inline void pixel_be(uint8_t *where, uint16_t color) {
    where[0] = (uint8_t)(color >> 8);
    where[1] = (uint8_t)color;
}

static uint16_t rgb565(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) | (b >> 3));
}

static void record_failure(void) {
    if (stats.failures != UINT32_MAX) stats.failures++;
}

static void record_ui_result(esp_err_t result) {
    if (result != ESP_OK) record_failure();
}

static uint16_t power_color(unsigned level) {
    if (level > 255u) level = 255u;
    const unsigned stop_count = sizeof(WATERFALL_COLORS) / sizeof(WATERFALL_COLORS[0]);
    for (unsigned i = 0; i + 1u < stop_count; i++) {
        const waterfall_color_stop_t *low = &WATERFALL_COLORS[i];
        const waterfall_color_stop_t *high = &WATERFALL_COLORS[i + 1u];
        if (level > high->level) continue;
        unsigned width = (unsigned)high->level - low->level;
        unsigned offset = level - low->level;
        int red = (int)low->red + ((int)high->red - low->red) * (int)offset / (int)width;
        int green = (int)low->green + ((int)high->green - low->green) * (int)offset / (int)width;
        int blue = (int)low->blue + ((int)high->blue - low->blue) * (int)offset / (int)width;
        return rgb565((unsigned)red, (unsigned)green, (unsigned)blue);
    }
    const waterfall_color_stop_t *last = &WATERFALL_COLORS[stop_count - 1u];
    return rgb565(last->red, last->green, last->blue);
}

static esp_err_t tx_raw(const void *data, size_t size, bool data_mode) {
    if (!size) return ESP_OK;
    gpio_set_level(PIN_LCD_DC, data_mode ? 1 : 0);
    spi_transaction_t t = { .length = size * 8u, .tx_buffer = data };
    return spi_device_polling_transmit(lcd, &t);
}

static esp_err_t command(uint8_t cmd, const void *params, size_t nparams) {
    gpio_set_level(PIN_LCD_DC, 0);
    spi_transaction_t t = { .length = 8, .flags = SPI_TRANS_USE_TXDATA };
    t.tx_data[0] = cmd;
    esp_err_t err = spi_device_polling_transmit(lcd, &t);
    if (err != ESP_OK) return err;
    return tx_raw(params, nparams, true);
}

static esp_err_t write_window(unsigned x, unsigned y, unsigned width,
                              unsigned height, const uint8_t *pixels,
                              size_t bytes) {
    if (!width || !height || x + width > LCD_W || y + height > LCD_H) return ESP_ERR_INVALID_ARG;
    /* TFT_eSPI auto-enables CGRAM_OFFSET for 170x320 panels. Its rotation-3
     * table uses rowstart=35; rotation maps that portrait column gap to Y. */
    const unsigned y_offset = 35u;
    uint8_t col[4] = { (uint8_t)(x >> 8), (uint8_t)x,
                       (uint8_t)((x + width - 1) >> 8), (uint8_t)(x + width - 1) };
    unsigned y0 = y + y_offset, y1 = y0 + height - 1;
    uint8_t row[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0,
                       (uint8_t)(y1 >> 8), (uint8_t)y1 };
    esp_err_t err = command(0x2a, col, sizeof(col));
    if (err != ESP_OK) return err;
    err = command(0x2b, row, sizeof(row));
    if (err != ESP_OK) return err;
    gpio_set_level(PIN_LCD_DC, 0);
    spi_transaction_t start = { .length = 8, .flags = SPI_TRANS_USE_TXDATA };
    start.tx_data[0] = 0x2c;
    err = spi_device_polling_transmit(lcd, &start);
    if (err != ESP_OK) return err;
    return tx_raw(pixels, bytes, true);
}

static esp_err_t draw_row(unsigned y, const uint8_t *pixels) {
    return write_window(0, y, LCD_W, 1, pixels, LCD_ROW_BYTES);
}

void t_embed_ui_set_frame_flush_suppressed(bool suppressed) {
    frame_flush_suppressed = suppressed;
}

void t_embed_ui_flush_framebuffer(const uint16_t *pixels, unsigned y0, unsigned y1) {
    if (!stats.ready || !pixels || y0 >= LCD_H) return;
    if (y1 > LCD_H) y1 = LCD_H;
    if (y1 <= y0) return;
    for (unsigned y = y0; y < y1;) {
        unsigned rows = y1 - y;
        if (rows > STRIP_ROWS) rows = STRIP_ROWS;
        memcpy(dma_strip, (const uint8_t *)pixels + y * LCD_ROW_BYTES,
               rows * LCD_ROW_BYTES);
        record_ui_result(write_window(0, y, LCD_W, rows, dma_strip,
                                      rows * LCD_ROW_BYTES));
        y += rows;
    }
}

void t_embed_ui_display_flush(const uint16_t *pixels, unsigned y0, unsigned y1) {
    if (!frame_flush_suppressed) t_embed_ui_flush_framebuffer(pixels, y0, y1);
}

static void put_text_scanline(int x, unsigned text_y, unsigned screen_y,
                              const char *text, uint16_t color);

void t_embed_ui_render_menu_overlay(const char *title, const char *const *items,
                                    unsigned count, unsigned selected,
                                    const char *hint) {
    if (!stats.ready || !title || !items || !count) return;
    if (count > 6u) count = 6u;
    selected %= count;
    if (overlay_cache_valid && overlay_title == title && overlay_items == items &&
        overlay_count == count && overlay_selection == selected && overlay_hint == hint)
        return;
    overlay_cache_valid = true;
    overlay_title = title;
    overlay_items = items;
    overlay_count = count;
    overlay_selection = selected;
    overlay_hint = hint;
    prompt_overlay_valid = false;
    for (unsigned y = 0; y < LCD_H; y++) {
        uint16_t background = y < 25u ? COLOR_HEADER : COLOR_BG;
        for (unsigned x = 0; x < LCD_W; x++) pixel_be(line + 2u * x, background);
        if (y == 24u) {
            for (unsigned x = 0; x < LCD_W; x++) pixel_be(line + 2u * x, COLOR_GRID);
        }
        put_text_scanline(8, 8, y, title, COLOR_ACCENT);
        for (unsigned i = 0; i < count; i++) {
            unsigned row_height = count > 5u ? 18u : 22u;
            unsigned first_y = 32u + i * row_height;
            unsigned last_y = first_y + row_height - 2u;
            if (y >= first_y && y < last_y) {
                uint16_t fill = i == selected ? COLOR_HEADER : COLOR_BG;
                for (unsigned x = 16u; x < LCD_W - 16u; x++)
                    pixel_be(line + 2u * x, fill);
            }
            if (y == first_y || y + 1u == last_y) {
                uint16_t edge = i == selected ? COLOR_ACCENT : COLOR_GRID;
                for (unsigned x = 16u; x < LCD_W - 16u; x++)
                    pixel_be(line + 2u * x, edge);
            }
            if (y >= first_y + 7u && y < first_y + 14u)
                put_text_scanline(28, first_y + 7u, y, items[i],
                                  i == selected ? COLOR_ACCENT : COLOR_TEXT);
        }
        put_text_scanline(8, 153, y, hint ? hint : "TURN SELECT PRESS OPEN", COLOR_DIM);
        record_ui_result(draw_row(y, line));
    }
}

void t_embed_ui_invalidate_overlays(void) {
    overlay_cache_valid = false;
    prompt_overlay_valid = false;
}

static void glyph(char c, uint8_t out[5]) {
    /* Five-column, seven-row font. Only uppercase UI labels and numerals are
     * needed, which keeps the firmware independent of a font component. */
    static const uint8_t digits[10][5] = {
        {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},
        {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
        {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
        {0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
        {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e}
    };
    static const uint8_t letters[26][5] = {
        {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},
        {0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},
        {0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},
        {0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},
        {0x00,0x41,0x7f,0x41,0x00},{0x20,0x40,0x41,0x3f,0x01},
        {0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
        {0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},
        {0x3e,0x41,0x41,0x41,0x3e},{0x7f,0x09,0x09,0x09,0x06},
        {0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},
        {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},
        {0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},
        {0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,0x08,0x14,0x63},
        {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}
    };
    memset(out, 0, 5);
    if (c >= '0' && c <= '9') memcpy(out, digits[c - '0'], 5);
    else if (c >= 'A' && c <= 'Z') memcpy(out, letters[c - 'A'], 5);
    else if (c == '-') { out[1] = out[2] = out[3] = 0x08; }
    else if (c == '=') { out[1] = out[2] = out[3] = 0x14; }
    else if (c == '.') { out[2] = 0x60; }
    else if (c == ',') { out[2] = 0x60; out[3] = 0x40; }
    else if (c == ':') { out[2] = 0x36; }
    else if (c == '!') { out[2] = 0x5f; }
    else if (c == '?') { out[0] = 0x02; out[1] = 0x01; out[2] = 0x51; out[3] = 0x09; out[4] = 0x06; }
    else if (c == '/') { out[0] = 0x40; out[1] = 0x30; out[2] = 0x0c; out[3] = 0x03; out[4] = 0x01; }
    else if (c == '\'') { out[2] = 0x07; }
}

static void put_text_scanline(int x, unsigned text_y, unsigned screen_y,
                              const char *text, uint16_t color) {
    if (screen_y < text_y || screen_y >= text_y + 7u) return;
    unsigned row = screen_y - text_y;
    while (*text && x + 5 < (int)LCD_W) {
        uint8_t cols[5];
        glyph(*text++, cols);
        for (unsigned col = 0; col < 5; col++) {
            if (cols[col] & (1u << row)) pixel_be(line + 2u * (unsigned)(x + (int)col), color);
        }
        x += 6;
    }
}

#define PROMPT_LINE_CHARS 52u
#define PROMPT_VISIBLE_LINES 7u

static char prompt_display_char(unsigned char value) {
    if (value >= 'a' && value <= 'z') return (char)(value - 'a' + 'A');
    if ((value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
        value == ' ' || value == '-' || value == '=' || value == '.' ||
        value == ',' || value == ':' || value == '!' || value == '?' ||
        value == '/')
        return (char)value;
    return ' ';
}

static unsigned prompt_line_count(const char *text) {
    unsigned lines = 1u;
    unsigned column = 0u;
    if (!text) return lines;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == '\r') continue;
        if (*p == '\n') {
            lines++;
            column = 0u;
            continue;
        }
        if ((*p & 0xc0u) == 0x80u) continue;
        if (column == PROMPT_LINE_CHARS) {
            lines++;
            column = 0u;
        }
        column++;
    }
    return lines;
}

unsigned t_embed_ui_prompt_max_scroll(const char *text) {
    unsigned lines = prompt_line_count(text);
    return lines > PROMPT_VISIBLE_LINES ? lines - PROMPT_VISIBLE_LINES : 0u;
}

static uint32_t prompt_text_hash(const char *title, const char *text) {
    uint32_t hash = 2166136261u;
    const char *parts[] = { title ? title : "", text ? text : "" };
    for (unsigned part = 0; part < 2u; part++) {
        for (const unsigned char *p = (const unsigned char *)parts[part]; *p; p++) {
            hash ^= *p;
            hash *= 16777619u;
        }
        hash ^= 0xffu;
        hash *= 16777619u;
    }
    return hash;
}

static void prompt_visible_lines(const char *text, unsigned first_line,
                                 char lines[PROMPT_VISIBLE_LINES][PROMPT_LINE_CHARS + 1u]) {
    memset(lines, 0, PROMPT_VISIBLE_LINES * (PROMPT_LINE_CHARS + 1u));
    unsigned line_index = 0u;
    unsigned column = 0u;
    if (!text) return;
    for (const unsigned char *p = (const unsigned char *)text; ; p++) {
        bool end = *p == '\0';
        if (end || *p == '\r' || *p == '\n' || column == PROMPT_LINE_CHARS) {
            if (line_index >= first_line && line_index - first_line < PROMPT_VISIBLE_LINES)
                lines[line_index - first_line][column] = '\0';
            if (end) break;
            if (*p == '\r') continue;
            line_index++;
            column = 0u;
            if (*p == '\n') continue;
        }
        if ((*p & 0xc0u) == 0x80u) continue;
        if (line_index >= first_line && line_index - first_line < PROMPT_VISIBLE_LINES)
            lines[line_index - first_line][column] = prompt_display_char(*p);
        column++;
    }
}

void t_embed_ui_render_prompt_overlay(const char *title, const char *text,
                                      unsigned scroll, bool answer_view,
                                      unsigned selected, unsigned outcome) {
    if (!stats.ready) return;
    if (!title) title = "HERMES ASKS";
    if (!text) text = "";
    unsigned max_scroll = t_embed_ui_prompt_max_scroll(text);
    if (scroll > max_scroll) scroll = max_scroll;
    selected %= 2u;
    uint32_t hash = prompt_text_hash(title, text);
    if (prompt_overlay_valid && prompt_overlay_hash == hash &&
        prompt_overlay_scroll == scroll && prompt_overlay_answer_view == answer_view &&
        prompt_overlay_selection == selected && prompt_overlay_outcome == outcome)
        return;
    prompt_overlay_valid = true;
    prompt_overlay_hash = hash;
    prompt_overlay_scroll = scroll;
    prompt_overlay_answer_view = answer_view;
    prompt_overlay_selection = selected;
    prompt_overlay_outcome = outcome;
    overlay_cache_valid = false;

    char visible[PROMPT_VISIBLE_LINES][PROMPT_LINE_CHARS + 1u];
    prompt_visible_lines(text, scroll, visible);
    char heading[53];
    size_t title_length = strlen(title);
    if (title_length > 52u) title_length = 52u;
    for (size_t i = 0; i < title_length; i++) heading[i] = prompt_display_char((unsigned char)title[i]);
    heading[title_length] = '\0';

    for (unsigned y = 0; y < LCD_H; y++) {
        uint16_t background = y < 25u ? COLOR_HEADER : COLOR_BG;
        for (unsigned x = 0; x < LCD_W; x++) pixel_be(line + 2u * x, background);
        if (y == 24u) {
            for (unsigned x = 0; x < LCD_W; x++) pixel_be(line + 2u * x, COLOR_GRID);
        }
        put_text_scanline(8, 8, y, heading, COLOR_ACCENT);
        for (unsigned i = 0; i < PROMPT_VISIBLE_LINES; i++)
            put_text_scanline(8, 34u + i * 13u, y, visible[i], COLOR_TEXT);
        if (outcome != 0u) {
            const char *message = outcome == 1u ? "ANSWER QUEUED WAITING" :
                                  outcome == 2u ? "QUESTION EXPIRED" :
                                  outcome == 4u ? "WAIT FOR ARMING WINDOW" :
                                  "REPLY FAILED RECONNECT";
            put_text_scanline(8, 153, y, message, COLOR_DIM);
        } else if (answer_view) {
            for (unsigned i = 0; i < 2u; i++) {
                unsigned first_x = i == 0u ? 32u : 168u;
                unsigned last_x = i == 0u ? 152u : 288u;
                uint16_t fill = i == selected ? COLOR_GRID : COLOR_HEADER;
                for (unsigned x = first_x; x < last_x; x++) pixel_be(line + 2u * x, fill);
                if (y == 140u || y == 157u) {
                    uint16_t edge = i == selected ? COLOR_ACCENT : COLOR_DIM;
                    for (unsigned x = first_x; x < last_x; x++) pixel_be(line + 2u * x, edge);
                }
                if (y >= 146u && y < 153u)
                    put_text_scanline((int)first_x + 12, 146, y,
                                      i == 0u ? "DENY" : "APPROVE",
                                      i == selected ? COLOR_ACCENT : COLOR_TEXT);
            }
            put_text_scanline(8, 160, y, "TURN SELECT PRESS CONFIRM", COLOR_DIM);
        } else {
            put_text_scanline(8, 153, y, "TURN SCROLL PRESS OPTIONS", COLOR_DIM);
        }
        record_ui_result(draw_row(y, line));
    }
}

static void put_text_scanline_scaled(int x, unsigned text_y, unsigned screen_y,
                                     const char *text, uint16_t color,
                                     unsigned scale) {
    if (!scale || screen_y < text_y || screen_y >= text_y + 7u * scale) return;
    unsigned row = (screen_y - text_y) / scale;
    while (*text && x + (int)(5u * scale) <= (int)LCD_W) {
        uint8_t cols[5];
        glyph(*text++, cols);
        for (unsigned col = 0; col < 5; col++) {
            if (cols[col] & (1u << row)) {
                for (unsigned dx = 0; dx < scale; dx++) {
                    unsigned pixel_x = (unsigned)x + col * scale + dx;
                    pixel_be(line + 2u * pixel_x, color);
                }
            }
        }
        x += (int)(6u * scale);
    }
}

static esp_err_t draw_status_bars(const t_embed_ui_view_t *view) {
    char frequency[12], span[16], step[16];
    snprintf(frequency, sizeof(frequency), "%u", view->frequency_mhz);
    unsigned span_mhz = view->rate_code == 6 ? 16u : view->rate_code == 0 ? 80u : 40u;
    snprintf(span, sizeof(span), "SPAN %u", span_mhz);
    snprintf(step, sizeof(step), "STEP %u MHZ", view->step_mhz);
    size_t frequency_len = strlen(frequency);
    int frequency_x = 61 + (frequency_len < 4u ? (int)((4u - frequency_len) * 6u) : 0);

    for (unsigned y = 0; y < HEADER_H; y++) {
        for (unsigned x = 0; x < LCD_W; x++) pixel_be(line + x * 2u, COLOR_HEADER);
        if (y == HEADER_H - 1u) {
            for (unsigned x = 0; x < LCD_W; x++) pixel_be(line + x * 2u, COLOR_GRID);
        }
        put_text_scanline(6, 5, y, "ESP-SDR", COLOR_DIM);
        put_text_scanline_scaled(frequency_x, 3, y, frequency, COLOR_TEXT, 2);
        put_text_scanline(112, 9, y, "MHZ", COLOR_DIM);
        put_text_scanline(154, 9, y, span, COLOR_TEXT);
        put_text_scanline(277, 9, y, view->paused ? "PAUSE" : "LIVE",
                          view->paused ? COLOR_PEAK : COLOR_ACCENT);
        put_text_scanline(6, 15, y, step, COLOR_DIM);
        esp_err_t err = draw_row(y, line);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static esp_err_t draw_axis(const t_embed_ui_view_t *view) {
    unsigned span = view->rate_code == 6 ? 16u : view->rate_code == 0 ? 80u : 40u;
    char left[12], right[12];
    int left_mhz = (int)view->frequency_mhz - (int)(span / 2u);
    int right_mhz = (int)view->frequency_mhz + (int)(span / 2u);
    snprintf(left, sizeof(left), "%d", left_mhz);
    snprintf(right, sizeof(right), "%d", right_mhz);
    for (unsigned y = AXIS_Y; y < WATER_Y; y++) {
        for (unsigned x = 0; x < LCD_W; x++) pixel_be(line + x * 2u, COLOR_BG);
        put_text_scanline(2, AXIS_Y, y, left, COLOR_DIM);
        put_text_scanline((int)LCD_W - 2 - (int)strlen(right) * 6, AXIS_Y, y,
                          right, COLOR_DIM);
        esp_err_t err = draw_row(y, line);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static esp_err_t draw_footer(void) {
    for (unsigned y = FOOTER_Y; y < FOOTER_Y + FOOTER_H; y++) {
        for (unsigned x = 0; x < LCD_W; x++) pixel_be(line + x * 2u, COLOR_HEADER);
        for (unsigned x = 42; x < 144; x++) {
            unsigned level = (x - 42u) * 255u / 101u;
            pixel_be(line + x * 2u, power_color(level));
        }
        put_text_scanline(6, FOOTER_Y + 2u, y, "LOW", COLOR_DIM);
        put_text_scanline(158, FOOTER_Y + 2u, y, "REL PWR", COLOR_TEXT);
        put_text_scanline(281, FOOTER_Y + 2u, y, "HIGH", COLOR_DIM);
        esp_err_t err = draw_row(y, line);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static esp_err_t draw_graph(const uint8_t *bins, unsigned nfft) {
    unsigned peak = 0;
    if (bins && nfft) for (unsigned i = 0; i < nfft; i++) if (bins[i] > peak) peak = bins[i];
    unsigned floor = peak > 64u ? peak - 64u : 0u;
    unsigned range = peak > floor ? peak - floor : 1u;
    for (unsigned x = 0; x < LCD_W; x++) {
        unsigned first = (uint64_t)x * nfft / LCD_W;
        unsigned end = (uint64_t)(x + 1u) * nfft / LCD_W;
        if (end <= first) end = first + 1u;
        unsigned value = 0;
        for (unsigned k = first; k < end && k < nfft; k++) {
            unsigned sample = bins && nfft ? bins[(k + nfft / 2u) % nfft] : 0;
            if (sample > value) value = sample;
        }
        unsigned relative = value > floor ? value - floor : 0;
        if (relative > range) relative = range;
        trace_y[x] = (uint8_t)(GRAPH_Y + GRAPH_H - 1u - relative * (GRAPH_H - 1u) / range);
    }

    for (unsigned gy = 0; gy < GRAPH_H; gy++) {
        unsigned y = GRAPH_Y + gy;
        for (unsigned x = 0; x < LCD_W; x++) {
            uint16_t color = ((gy == 0u || gy == GRAPH_H / 2u || gy == GRAPH_H - 1u) || x % 80u == 0)
                                 ? COLOR_GRID : COLOR_BG;
            if (bins && (trace_y[x] == y || (trace_y[x] > 0u && trace_y[x] - 1u == y)))
                color = COLOR_ACCENT;
            pixel_be(line + x * 2u, color);
        }
        esp_err_t err = draw_row(y, line);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static void make_waterfall_row(const uint8_t *bins, unsigned nfft, uint8_t *row) {
    unsigned peak = 0;
    for (unsigned i = 0; i < nfft; i++) if (bins[i] > peak) peak = bins[i];
    unsigned floor = peak > 64u ? peak - 64u : 0u;
    for (unsigned x = 0; x < LCD_W; x++) {
        unsigned first = (uint64_t)x * nfft / LCD_W;
        unsigned end = (uint64_t)(x + 1u) * nfft / LCD_W;
        if (end <= first) end = first + 1u;
        unsigned value = 0;
        for (unsigned k = first; k < end && k < nfft; k++) {
            unsigned sample = bins[(k + nfft / 2u) % nfft];
            if (sample > value) value = sample;
        }
        unsigned relative = value > floor ? value - floor : 0u;
        if (relative > 64u) relative = 64u;
        uint16_t color = power_color(relative * 255u / 64u);
        pixel_be(row + x * 2u, color);
    }
}

static esp_err_t draw_waterfall(void) {
    unsigned shown = 0;
    const unsigned row_bytes = LCD_ROW_BYTES;
    while (shown < WATER_H) {
        unsigned src = (water_head + shown) % WATER_H;
        unsigned rows = WATER_H - shown;
        if (rows > STRIP_ROWS) rows = STRIP_ROWS;
        if (rows > WATER_H - src) rows = WATER_H - src;
        memcpy(dma_strip, waterfall + src * row_bytes, rows * row_bytes);
        esp_err_t err = write_window(0, WATER_Y + shown, LCD_W, rows, dma_strip,
                                     rows * row_bytes);
        if (err != ESP_OK) return err;
        shown += rows;
    }
    return ESP_OK;
}

static esp_err_t send_init_sequence(void) {
    /* TFT_eSPI Setup210 rotation 3: MY | MV | RGB (no BGR bit). */
    const uint8_t colmod = 0x55, madctl = 0xa0;
    const uint8_t b2[] = {0x0b,0x0b,0x00,0x33,0x33};
    const uint8_t b7 = 0x75, bb = 0x28, c0 = 0x2c, c2 = 0x01, c3 = 0x1f;
    const uint8_t c6 = 0x13, d0a = 0xa7, d0b[] = {0xa4,0xa1}, d6 = 0xa1;
    const uint8_t e0[] = {0xf0,0x05,0x0a,0x06,0x06,0x03,0x2b,0x32,0x43,0x36,0x11,0x10,0x2b,0x32};
    const uint8_t e1[] = {0xf0,0x08,0x0c,0x0b,0x09,0x24,0x2b,0x22,0x43,0x38,0x15,0x16,0x2f,0x37};
    esp_err_t err = command(0x11, NULL, 0); /* Official T-Embed sleep-out. */
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));
    if ((err = command(0x3a, &colmod, 1)) != ESP_OK) return err;
    if ((err = command(0xb2, b2, sizeof(b2))) != ESP_OK) return err;
    if ((err = command(0xb7, &b7, 1)) != ESP_OK) return err;
    if ((err = command(0xbb, &bb, 1)) != ESP_OK) return err;
    if ((err = command(0xc0, &c0, 1)) != ESP_OK) return err;
    if ((err = command(0xc2, &c2, 1)) != ESP_OK) return err;
    if ((err = command(0xc3, &c3, 1)) != ESP_OK) return err;
    if ((err = command(0xc6, &c6, 1)) != ESP_OK) return err;
    if ((err = command(0xd0, &d0a, 1)) != ESP_OK) return err;
    if ((err = command(0xd0, d0b, sizeof(d0b))) != ESP_OK) return err;
    if ((err = command(0xd6, &d6, 1)) != ESP_OK) return err;
    if ((err = command(0xe0, e0, sizeof(e0))) != ESP_OK) return err;
    if ((err = command(0xe1, e1, sizeof(e1))) != ESP_OK) return err;
    if ((err = command(0x36, &madctl, 1)) != ESP_OK) return err;
    if ((err = command(0x21, NULL, 0)) != ESP_OK) return err; /* T-Embed inversion on. */
    if ((err = command(0x13, NULL, 0)) != ESP_OK) return err;
    err = command(0x29, NULL, 0);
    if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
    return err;
}

static bool init_encoder_counter(void) {
    bool unit_enabled = false;
    bool unit_started = false;
    pcnt_unit_config_t unit_config = {
        .high_limit = 30000,
        .low_limit = -30000,
    };
    esp_err_t err = pcnt_new_unit(&unit_config, &encoder_pcnt);
    if (err != ESP_OK) goto failed;

    pcnt_glitch_filter_config_t filter = { .max_glitch_ns = 1000 };
    if ((err = pcnt_unit_set_glitch_filter(encoder_pcnt, &filter)) != ESP_OK) goto failed;

    pcnt_chan_config_t channel_a = {
        .edge_gpio_num = PIN_ENCODER_A,
        .level_gpio_num = PIN_ENCODER_B,
    };
    if ((err = pcnt_new_channel(encoder_pcnt, &channel_a, &encoder_channel_a)) != ESP_OK) goto failed;
    pcnt_chan_config_t channel_b = {
        .edge_gpio_num = PIN_ENCODER_B,
        .level_gpio_num = PIN_ENCODER_A,
    };
    if ((err = pcnt_new_channel(encoder_pcnt, &channel_b, &encoder_channel_b)) != ESP_OK) goto failed;

    /* Both edges, with the opposite phase selecting direction, gives X4
     * quadrature counting and preserves turns while CPU interrupts are masked. */
    if ((err = pcnt_channel_set_edge_action(encoder_channel_a,
                PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE)) != ESP_OK) goto failed;
    if ((err = pcnt_channel_set_level_action(encoder_channel_a,
                PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE)) != ESP_OK) goto failed;
    if ((err = pcnt_channel_set_edge_action(encoder_channel_b,
                PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE)) != ESP_OK) goto failed;
    if ((err = pcnt_channel_set_level_action(encoder_channel_b,
                PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE)) != ESP_OK) goto failed;
    if ((err = pcnt_unit_enable(encoder_pcnt)) != ESP_OK) goto failed;
    unit_enabled = true;
    if ((err = pcnt_unit_clear_count(encoder_pcnt)) != ESP_OK) goto failed;
    if ((err = pcnt_unit_start(encoder_pcnt)) != ESP_OK) goto failed;
    unit_started = true;
    encoder_pcnt_ready = true;
    return true;

failed:
    if (unit_started) (void)pcnt_unit_stop(encoder_pcnt);
    if (unit_enabled) (void)pcnt_unit_disable(encoder_pcnt);
    if (encoder_channel_b) (void)pcnt_del_channel(encoder_channel_b);
    if (encoder_channel_a) (void)pcnt_del_channel(encoder_channel_a);
    if (encoder_pcnt) (void)pcnt_del_unit(encoder_pcnt);
    encoder_channel_a = encoder_channel_b = NULL;
    encoder_pcnt = NULL;
    record_failure();
    return false;
}

static int poll_encoder_detents(void) {
    if (encoder_pcnt_ready) {
        int count = 0;
        if (pcnt_unit_get_count(encoder_pcnt, &count) == ESP_OK &&
            pcnt_unit_clear_count(encoder_pcnt) == ESP_OK) {
            encoder_edges += count;
        } else {
            encoder_pcnt_ready = false;
            record_failure();
        }
    }
    int detents = 0;
    while (encoder_edges >= 2) {
        encoder_edges -= 2;
        detents++;
    }
    while (encoder_edges <= -2) {
        encoder_edges += 2;
        detents--;
    }
    return detents;
}

void t_embed_ui_poll_input(t_embed_input_context_t context, uint32_t hold_ms,
                           t_embed_input_event_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!stats.ready) return;
    int64_t now = esp_timer_get_time();
    bool raw_down = gpio_get_level(PIN_ENCODER_KEY) == 0;
    if (!gesture_initialized) {
        t_embed_gesture_init(&gesture, raw_down, now);
        gesture_initialized = true;
    }
    t_embed_gesture_event_t event;
    t_embed_gesture_update(&gesture, raw_down, poll_encoder_detents(), now,
                           30u, hold_ms, (uint8_t)context, &event);

    out->detents = event.detents;
    out->press_started = event.press_started;
    out->hold = event.hold;
    out->released = event.released;
    out->short_click = event.short_click;
    out->down = event.down;
    out->press_context = (t_embed_input_context_t)event.press_context;
    out->pressed_at_us = event.pressed_at_us;
}

static void apa102_write_byte(uint8_t value) {
    for (uint8_t mask = 0x80u; mask; mask >>= 1) {
        gpio_set_level(PIN_LED_CLOCK, 0);
        gpio_set_level(PIN_LED_DATA, (value & mask) != 0);
        gpio_set_level(PIN_LED_CLOCK, 1);
    }
}

void t_embed_ui_set_microphone_leds(bool enabled, bool muted) {
    if (!mic_leds_ready ||
        (mic_led_state_known && mic_led_enabled == enabled && mic_led_muted == muted)) return;

    for (unsigned i = 0; i < 4u; i++) apa102_write_byte(0);
    for (unsigned i = 0; i < LED_COUNT; i++) {
        apa102_write_byte(enabled ? (uint8_t)(0xe0u | LED_DIM_BRIGHTNESS) : 0xe0u);
        apa102_write_byte(enabled && !muted ? 32u : 0u); /* blue */
        apa102_write_byte(enabled && !muted ? 20u : 0u); /* green */
        apa102_write_byte(enabled && muted ? 48u : 0u); /* red */
    }
    apa102_write_byte(0xffu);
    gpio_set_level(PIN_LED_CLOCK, 0);
    mic_led_enabled = enabled;
    mic_led_muted = muted;
    mic_led_state_known = true;
}

bool t_embed_ui_poll_microphone_mute(void) {
    if (!stats.ready || !mic_screen_drawn) return false;
    t_embed_input_event_t event;
    if (microphone_menu_active) {
        static const char *const items[] = { "HOME", "BACK" };
        t_embed_ui_poll_input(T_EMBED_INPUT_MICROPHONE_MENU, 0u, &event);
        if (microphone_menu_wait_release) {
            if (event.released) microphone_menu_wait_release = false;
            t_embed_ui_render_menu_overlay("USB MICROPHONE", items, 2u,
                                           microphone_menu_selection,
                                           "TURN SELECT PRESS OPEN");
            return false;
        }
        if (event.detents) microphone_menu_selection =
            t_embed_menu_move(microphone_menu_selection, event.detents, 2u);
        t_embed_ui_render_menu_overlay("USB MICROPHONE", items, 2u,
                                       microphone_menu_selection,
                                       "TURN SELECT PRESS OPEN");
        if (event.short_click && event.press_context == T_EMBED_INPUT_MICROPHONE_MENU) {
            if (microphone_menu_selection == 0u) home_requested = true;
            else {
                microphone_menu_active = false;
                mic_screen_drawn = false;
                t_embed_ui_invalidate_overlays();
            }
        }
        return false;
    }

    t_embed_ui_poll_input(T_EMBED_INPUT_MICROPHONE, 800u, &event);
    if (event.hold && event.press_context == T_EMBED_INPUT_MICROPHONE) {
        microphone_menu_active = true;
        microphone_menu_selection = 0;
        microphone_menu_wait_release = true;
        t_embed_gesture_suppress_until_release(&gesture);
        t_embed_ui_invalidate_overlays();
        return false;
    }
    return event.short_click && event.press_context == T_EMBED_INPUT_MICROPHONE;
}

bool t_embed_ui_init(unsigned initial_frequency_mhz) {
    stats = (t_embed_ui_stats_t){0};
    stats.frequency_mhz = initial_frequency_mhz;
    stats.rate_code = rate_code;
    stats.step_mhz = step_mhz;

    waterfall = heap_caps_malloc(WATER_H * LCD_ROW_BYTES,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!waterfall) waterfall = heap_caps_malloc(WATER_H * LCD_ROW_BYTES,
                                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    dma_strip = heap_caps_malloc(STRIP_ROWS * LCD_ROW_BYTES,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!waterfall || !dma_strip) { record_failure(); return false; }
    memset(waterfall, 0, WATER_H * LCD_ROW_BYTES);
    memset(dma_strip, 0, STRIP_ROWS * LCD_ROW_BYTES);

    const uint64_t outputs = (1ull << PIN_LCD_DC) | (1ull << PIN_LCD_RST) |
                             (1ull << PIN_LCD_BL) | (1ull << PIN_POWER_ON) |
                             (1ull << PIN_LED_DATA) | (1ull << PIN_LED_CLOCK);
    gpio_config_t out = { .pin_bit_mask = outputs, .mode = GPIO_MODE_OUTPUT,
                          .pull_up_en = GPIO_PULLUP_DISABLE,
                          .pull_down_en = GPIO_PULLDOWN_DISABLE,
                          .intr_type = GPIO_INTR_DISABLE };
    if (gpio_config(&out) != ESP_OK) { record_failure(); return false; }
    mic_leds_ready = true;
    mic_led_state_known = false;
    t_embed_ui_set_microphone_leds(false, false);
    const uint64_t inputs = (1ull << PIN_ENCODER_A) | (1ull << PIN_ENCODER_B) |
                            (1ull << PIN_ENCODER_KEY);
    gpio_config_t in = { .pin_bit_mask = inputs, .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE,
                         .pull_down_en = GPIO_PULLDOWN_DISABLE,
                         .intr_type = GPIO_INTR_DISABLE };
    if (gpio_config(&in) != ESP_OK) { record_failure(); return false; }
    /* Preserve the display if PCNT is unavailable; init records the error. */
    (void)init_encoder_counter();
    gpio_set_level(PIN_POWER_ON, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_LCD_BL, 0);
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    spi_bus_config_t bus = { .mosi_io_num = PIN_LCD_MOSI,
                             .miso_io_num = -1,
                             .sclk_io_num = PIN_LCD_SCLK,
                             .quadwp_io_num = -1,
                             .quadhd_io_num = -1,
                             .max_transfer_sz = STRIP_ROWS * LCD_ROW_BYTES };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        record_failure();
        return false;
    }
    spi_device_interface_config_t device = { .clock_speed_hz = LCD_SPI_HZ,
                                              .mode = 0,
                                              .spics_io_num = PIN_LCD_CS,
                                              .queue_size = 1 };
    if (spi_bus_add_device(SPI2_HOST, &device, &lcd) != ESP_OK) {
        record_failure();
        return false;
    }
    if (send_init_sequence() != ESP_OK) { record_failure(); return false; }
    gpio_set_level(PIN_LCD_BL, 1);

    int64_t now = esp_timer_get_time();
    t_embed_gesture_init(&gesture, gpio_get_level(PIN_ENCODER_KEY) == 0, now);
    gesture_initialized = true;
    encoder_edges = 0;
    sdr_menu_active = sdr_span_menu_active = microphone_menu_active = false;
    sdr_menu_wait_release = microphone_menu_wait_release = false;
    home_requested = false;
    frame_flush_suppressed = false;
    t_embed_ui_invalidate_overlays();
    stats.ready = true;
    next_capture_us = esp_timer_get_time();
    t_embed_ui_view_t initial = { .frequency_mhz = initial_frequency_mhz,
                                  .rate_code = rate_code,
                                  .step_mhz = step_mhz,
                                  .paused = false,
                                  .usb_transport = true };
    t_embed_ui_render_view(&initial);
    return true;
}

static void draw_launcher_row(unsigned y, unsigned selection) {
    const unsigned first_y[] = { 43u, 77u, 111u };
    const unsigned last_y[] = { 69u, 103u, 137u };
    const char *labels[] = { "ESP-SDR", "USB MICROPHONE", "HERMES" };
    unsigned selected = selection % 3u;
    for (unsigned x = 0; x < LCD_W; x++) pixel_be(line + 2u * x, COLOR_BG);
    for (unsigned i = 0; i < 3u; i++) {
        if (y >= first_y[i] && y < last_y[i]) {
            for (unsigned x = 20; x < LCD_W - 20u; x++)
                pixel_be(line + 2u * x, i == selected ? COLOR_HEADER : COLOR_BG);
        }
    }
    for (unsigned i = 0; i < 3u; i++) {
        if (y == first_y[i] || y + 1u == last_y[i]) {
            for (unsigned x = 20; x < LCD_W - 20u; x++)
                pixel_be(line + 2u * x, i == selected ? COLOR_ACCENT : COLOR_GRID);
        }
    }
    for (unsigned i = 0; i < 3u; i++) {
        if (y >= first_y[i] + 12u && y < first_y[i] + 19u) {
            const char *label = labels[i];
            int x = (int)(LCD_W - strlen(label) * 6u) / 2;
            put_text_scanline(x, first_y[i] + 12u, y, label,
                              i == selected ? COLOR_ACCENT : COLOR_DIM);
        }
    }
}

void t_embed_ui_render_launcher(unsigned selection) {
    if (!stats.ready) return;
    view_valid = false;
    mic_screen_drawn = false;
    for (unsigned y = 0; y < LCD_H; y++) {
        uint16_t background = y < 25u ? COLOR_HEADER : COLOR_BG;
        for (unsigned x = 0; x < LCD_W; x++) pixel_be(line + 2u * x, background);
        if (y == 24u) {
            for (unsigned x = 0; x < LCD_W; x++) pixel_be(line + 2u * x, COLOR_GRID);
        }
        put_text_scanline(8, 8, y, "T-EMBED APPS", COLOR_ACCENT);
        put_text_scanline(8, 34, y, "SELECT APP", COLOR_DIM);
        draw_launcher_row(y, selection);
        put_text_scanline(8, 139, y, "TURN TO SELECT", COLOR_DIM);
        put_text_scanline(8, 153, y, "PRESS TO OPEN", COLOR_TEXT);
        record_ui_result(draw_row(y, line));
    }
}

bool t_embed_ui_poll_launcher(unsigned *selection, bool *activate) {
    if (!stats.ready || !selection || !activate) return false;
    *activate = false;
    t_embed_input_event_t event;
    t_embed_ui_poll_input(T_EMBED_INPUT_LAUNCHER, 800u, &event);
    bool changed = event.detents != 0;
    if (changed) *selection = t_embed_menu_move(*selection, event.detents, 3u);
    *activate = event.short_click && event.press_context == T_EMBED_INPUT_LAUNCHER;
    return changed;
}

static void draw_microphone_status(bool connected) {
    for (unsigned y = 54u; y < 70u; y++) {
        for (unsigned x = 0; x < LCD_W; x++) pixel_be(line + 2u * x, COLOR_BG);
        put_text_scanline(8, 59, y, connected ? "BRIDGE CONNECTED" : "WAITING FOR BRIDGE",
                          connected ? COLOR_ACCENT : COLOR_DIM);
        record_ui_result(draw_row(y, line));
    }
}

static void draw_microphone_mute(bool muted) {
    for (unsigned y = 75u; y < 85u; y++) {
        for (unsigned x = 0; x < LCD_W; x++) pixel_be(line + 2u * x, COLOR_BG);
        put_text_scanline(8, 77, y, muted ? "MIC MUTED" : "MIC LIVE",
                          muted ? COLOR_MUTE : COLOR_ACCENT);
        record_ui_result(draw_row(y, line));
    }
}

void t_embed_ui_render_microphone(unsigned level, bool bridge_connected, bool muted) {
    if (!stats.ready || microphone_menu_active) return;
    bool first_draw = !mic_screen_drawn;
    if (!mic_screen_drawn) {
        view_valid = false;
        for (unsigned y = 0; y < LCD_H; y++) {
            uint16_t background = y < 25u ? COLOR_HEADER : COLOR_BG;
            for (unsigned x = 0; x < LCD_W; x++) pixel_be(line + 2u * x, background);
            if (y == 24u) {
                for (unsigned x = 0; x < LCD_W; x++) pixel_be(line + 2u * x, COLOR_GRID);
            }
            put_text_scanline(8, 8, y, "USB MIC BRIDGE", COLOR_ACCENT);
            put_text_scanline(8, 35, y, "16 KHZ MONO", COLOR_TEXT);
            put_text_scanline(8, 132, y, "PRESS KNOB TO MUTE", COLOR_DIM);
            put_text_scanline(8, 140, y, "HOLD KNOB FOR MENU", COLOR_DIM);
            record_ui_result(draw_row(y, line));
        }
        mic_screen_drawn = true;
        mic_bridge_connected = !bridge_connected;
    }
    if (bridge_connected != mic_bridge_connected) {
        draw_microphone_status(bridge_connected);
        mic_bridge_connected = bridge_connected;
    }
    if (first_draw || muted != mic_screen_muted) draw_microphone_mute(muted);
    mic_screen_muted = muted;
    t_embed_ui_set_microphone_leds(true, muted);
    unsigned fill = level > 255u ? 255u : level;
    unsigned width = fill * 256u / 255u;
    for (unsigned y = 92u; y < 110u; y++) {
        for (unsigned x = 32u; x < 288u; x++) {
            bool lit = x - 32u < width;
            pixel_be(line + 2u * x, lit ? (fill > 220u ? COLOR_PEAK : COLOR_ACCENT) : COLOR_HEADER);
        }
        record_ui_result(write_window(32u, y, 256u, 1u, line + 64u, 512u));
    }
}

bool t_embed_ui_home_requested(void) {
    return home_requested;
}

static void cycle_step(void) {
    step_mhz = step_mhz == 1u ? 5u : step_mhz == 5u ? 10u : 1u;
}

bool t_embed_ui_poll_controls(unsigned *frequency_mhz) {
    if (!stats.ready || !frequency_mhz) return false;
    bool changed = false;
    int64_t now = esp_timer_get_time();
    t_embed_input_event_t event;
    if (sdr_menu_active) {
        t_embed_ui_poll_input(T_EMBED_INPUT_SDR_MENU, 0u, &event);
        if (sdr_menu_wait_release) {
            if (event.released) sdr_menu_wait_release = false;
            if (sdr_span_menu_active) {
                static const char *const spans[] = { "16 MSPS", "40 MSPS", "80 MSPS" };
                t_embed_ui_render_menu_overlay("SELECT SPAN", spans, 3u,
                                               sdr_span_selection,
                                               "TURN SELECT PRESS SET");
            } else {
                static const char *const items[] = { "PAUSE / RESUME", "SPAN", "HOME", "BACK" };
                t_embed_ui_render_menu_overlay("SDR MENU", items, 4u,
                                               sdr_menu_selection,
                                               "TURN SELECT PRESS OPEN");
            }
            stats.paused = paused;
            stats.frequency_mhz = *frequency_mhz;
            stats.rate_code = rate_code;
            stats.step_mhz = step_mhz;
            return false;
        }
        if (event.detents) {
            if (sdr_span_menu_active)
                sdr_span_selection = t_embed_menu_move(sdr_span_selection,
                                                       event.detents, 3u);
            else
                sdr_menu_selection = t_embed_menu_move(sdr_menu_selection,
                                                       event.detents, 4u);
        }
        if (sdr_span_menu_active) {
            static const char *const spans[] = { "16 MSPS", "40 MSPS", "80 MSPS" };
            t_embed_ui_render_menu_overlay("SELECT SPAN", spans, 3u,
                                           sdr_span_selection,
                                           "TURN SELECT PRESS SET");
        } else {
            static const char *const items[] = { "PAUSE / RESUME", "SPAN", "HOME", "BACK" };
            t_embed_ui_render_menu_overlay("SDR MENU", items, 4u,
                                           sdr_menu_selection,
                                           "TURN SELECT PRESS OPEN");
        }
        if (event.short_click && event.press_context == T_EMBED_INPUT_SDR_MENU) {
            if (sdr_span_menu_active) {
                static const unsigned spans[] = { 6u, 1u, 0u };
                rate_code = spans[sdr_span_selection];
                sdr_span_menu_active = false;
                changed = true;
            } else if (sdr_menu_selection == 0u) {
                paused = !paused;
                next_capture_us = now;
                changed = true;
            } else if (sdr_menu_selection == 1u) {
                sdr_span_menu_active = true;
                sdr_span_selection = rate_code == 6u ? 0u : rate_code == 1u ? 1u : 2u;
            } else if (sdr_menu_selection == 2u) {
                home_requested = true;
            } else {
                sdr_menu_active = false;
                view_valid = false;
                t_embed_ui_invalidate_overlays();
            }
        }
        stats.paused = paused;
        stats.frequency_mhz = *frequency_mhz;
        stats.rate_code = rate_code;
        stats.step_mhz = step_mhz;
        return changed;
    }

    t_embed_ui_poll_input(T_EMBED_INPUT_SDR, 800u, &event);
    if (event.detents > 0) {
        for (int i = 0; i < event.detents; i++) {
            if (*frequency_mhz < 2483u) {
                unsigned next = *frequency_mhz + step_mhz;
                *frequency_mhz = next > 2483u ? 2483u : next;
                changed = true;
            }
        }
    } else if (event.detents < 0) {
        for (int i = 0; i > event.detents; i--) {
            if (*frequency_mhz > 2400u) {
                unsigned next = *frequency_mhz > 2400u + step_mhz
                                    ? *frequency_mhz - step_mhz : 2400u;
                *frequency_mhz = next;
                changed = true;
            }
        }
    }
    if (event.short_click && event.press_context == T_EMBED_INPUT_SDR) {
        cycle_step();
        changed = true;
    }
    if (event.hold && event.press_context == T_EMBED_INPUT_SDR) {
        sdr_menu_active = true;
        sdr_menu_selection = 0;
        sdr_menu_wait_release = true;
        t_embed_gesture_suppress_until_release(&gesture);
        t_embed_ui_invalidate_overlays();
    }
    stats.paused = paused;
    stats.frequency_mhz = *frequency_mhz;
    stats.rate_code = rate_code;
    stats.step_mhz = step_mhz;
    if (sdr_menu_active) {
        static const char *const items[] = { "PAUSE / RESUME", "SPAN", "HOME", "BACK" };
        t_embed_ui_render_menu_overlay("SDR MENU", items, 4u,
                                       sdr_menu_selection,
                                       "TURN SELECT PRESS OPEN");
    }
    return changed;
}

bool t_embed_ui_capture_due(int64_t now_us) {
    return stats.ready && !paused && now_us >= next_capture_us;
}

unsigned t_embed_ui_rate_code(void) { return rate_code; }
bool t_embed_ui_paused(void) { return paused; }

void t_embed_ui_note_capture(const ring_result_t *result, unsigned nfft,
                             unsigned frequency_mhz, unsigned selected_rate) {
    if (!result) return;
    /* These counters span bounded capture windows. Saturate instead of
     * wrapping so a long-running receiver still reports forward progress. */
    stats.rf_frames = UINT32_MAX - stats.rf_frames < result->frames
                          ? UINT32_MAX : stats.rf_frames + result->frames;
    stats.ffts = UINT32_MAX - stats.ffts < result->ffts
                     ? UINT32_MAX : stats.ffts + result->ffts;
    stats.capture_us = (uint32_t)result->elapsed_us;
    if (result->late_max > stats.late_max) stats.late_max = result->late_max;
    stats.drops = UINT32_MAX - stats.drops < result->drops
                      ? UINT32_MAX : stats.drops + result->drops;
    stats.nfft = nfft;
    stats.frequency_mhz = frequency_mhz;
    stats.rate_code = selected_rate;
    if (result->status && stats.failures != UINT32_MAX) stats.failures++;
    next_capture_us = esp_timer_get_time() + CAPTURE_GAP_US;
}

void t_embed_ui_render_view(const t_embed_ui_view_t *view) {
    if (!stats.ready || sdr_menu_active || !view) return;
    bool retune = !view_valid || last_view.frequency_mhz != view->frequency_mhz ||
                  last_view.rate_code != view->rate_code;
    bool redraw = retune || !view_valid || last_view.step_mhz != view->step_mhz ||
                  last_view.paused != view->paused;
    if (!redraw) return;
    stats.paused = view->paused;
    stats.frequency_mhz = view->frequency_mhz;
    stats.rate_code = view->rate_code;
    stats.step_mhz = view->step_mhz;
    if (retune) {
        memset(waterfall, 0, WATER_H * LCD_ROW_BYTES);
        water_head = 0;
        record_ui_result(draw_graph(NULL, 0));
        record_ui_result(draw_waterfall());
        record_ui_result(draw_axis(view));
    }
    record_ui_result(draw_status_bars(view));
    record_ui_result(draw_footer());
    last_view = *view;
    view_valid = true;
}

void t_embed_ui_render_spectrum(const uint8_t *bins, unsigned nfft) {
    if (!stats.ready || sdr_menu_active || !bins || nfft < 2u || nfft > RING_SPEC_NFFT_MAX) return;
    uint8_t min_bin = 255u, max_bin = 0u;
    uint32_t checksum = 2166136261u;
    for (unsigned i = 0; i < nfft; i++) {
        if (bins[i] < min_bin) min_bin = bins[i];
        if (bins[i] > max_bin) max_bin = bins[i];
        checksum = (checksum ^ bins[i]) * 16777619u;
    }
    make_waterfall_row(bins, nfft,
                       waterfall + water_head * LCD_ROW_BYTES);
    water_head = (water_head + 1u) % WATER_H;
    esp_err_t graph_result = draw_graph(bins, nfft);
    esp_err_t waterfall_result = draw_waterfall();
    record_ui_result(graph_result);
    record_ui_result(waterfall_result);
    stats.bin_min = min_bin;
    stats.bin_max = max_bin;
    stats.bin_checksum = checksum;
    if (graph_result == ESP_OK && waterfall_result == ESP_OK) {
        if (stats.screen_frames != UINT32_MAX) stats.screen_frames++;
        last_frame_us = esp_timer_get_time();
    }
}

void t_embed_ui_get_stats(t_embed_ui_stats_t *out) {
    if (!out) return;
    *out = stats;
    if (last_frame_us) {
        int64_t age = esp_timer_get_time() - last_frame_us;
        out->frame_age_ms = age > 0 ? (uint32_t)(age / 1000) : 0;
    } else {
        out->frame_age_ms = UINT32_MAX;
    }
}
