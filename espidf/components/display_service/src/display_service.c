#include "display_service.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sensor_service.h"
#include "tailnet_service.h"
#include "time_service.h"
#include "wifi_station.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "display_service";

#define LCD_H_RES 320
#define LCD_V_RES 170
#define LCD_PIXEL_CLOCK_HZ (16 * 1000 * 1000)
#define LCD_DRAW_ROWS 16
#define LCD_DRAW_PIXELS (LCD_H_RES * LCD_DRAW_ROWS)
#define LCD_DRAW_BUF_BYTES (LCD_DRAW_PIXELS * sizeof(uint16_t))

#define PIN_LCD_BL 38
#define PIN_LCD_D0 39
#define PIN_LCD_D1 40
#define PIN_LCD_D2 41
#define PIN_LCD_D3 42
#define PIN_LCD_D4 45
#define PIN_LCD_D5 46
#define PIN_LCD_D6 47
#define PIN_LCD_D7 48
#define PIN_LCD_RES 5
#define PIN_LCD_CS 6
#define PIN_LCD_DC 7
#define PIN_LCD_WR 8
#define PIN_LCD_RD 9
#define PIN_BUTTON_2 14

#define BUTTON_DEBOUNCE_MS 50
#define DISPLAY_REFRESH_MS 3000

#define RGB565(r, g, b) \
    (uint16_t)((((uint16_t)(r) & 0xF8) << 8) | \
               (((uint16_t)(g) & 0xFC) << 3) | \
               (((uint16_t)(b) & 0xF8) >> 3))

enum {
    COLOR_BLACK = RGB565(0, 0, 0),
    COLOR_PANEL = RGB565(16, 20, 28),
    COLOR_PANEL_2 = RGB565(28, 36, 48),
    COLOR_LINE = RGB565(92, 108, 122),
    COLOR_TEXT = RGB565(230, 238, 244),
    COLOR_MUTED = RGB565(150, 162, 174),
    COLOR_GREEN = RGB565(55, 210, 128),
    COLOR_YELLOW = RGB565(245, 190, 70),
    COLOR_RED = RGB565(230, 70, 70),
    COLOR_BLUE = RGB565(74, 150, 255),
    COLOR_CYAN = RGB565(50, 210, 220),
    COLOR_MAGENTA = RGB565(220, 80, 210),
    COLOR_WHITE = RGB565(255, 255, 255),
};

typedef struct {
    uint8_t cmd;
    uint8_t data[14];
    uint8_t len;
} lcd_cmd_t;

static const lcd_cmd_t lcd_st7789v_init[] = {
    {0x11, {0}, 0 | 0x80},
    {0x3A, {0x05}, 1},
    {0xB2, {0x0B, 0x0B, 0x00, 0x33, 0x33}, 5},
    {0xB7, {0x75}, 1},
    {0xBB, {0x28}, 1},
    {0xC0, {0x2C}, 1},
    {0xC2, {0x01}, 1},
    {0xC3, {0x1F}, 1},
    {0xC6, {0x13}, 1},
    {0xD0, {0xA7}, 1},
    {0xD0, {0xA4, 0xA1}, 2},
    {0xD6, {0xA1}, 1},
    {0xE0, {0xF0, 0x05, 0x0A, 0x06, 0x06, 0x03, 0x2B, 0x32,
            0x43, 0x36, 0x11, 0x10, 0x2B, 0x32}, 14},
    {0xE1, {0xF0, 0x08, 0x0C, 0x0B, 0x09, 0x24, 0x2B, 0x22,
            0x43, 0x38, 0x15, 0x16, 0x2F, 0x37}, 14},
};

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;
static TaskHandle_t s_task;
static uint16_t *s_draw_buf;
static char s_device_name[32] = "plain-air";
static uint8_t s_backlight_level;
static uint8_t s_brightness_index = 3;
static bool s_button_last_raw_pressed;
static bool s_button_stable_pressed;
static int64_t s_button_last_change_ms;
static uint32_t s_frame_count;

static const uint8_t s_brightness_levels[] = {0, 5, 11, 16};
static const uint8_t s_brightness_percents[] = {0, 30, 70, 100};

typedef struct {
    const char *label;
    uint16_t bg;
    uint16_t fg;
} air_quality_t;

static bool lcd_flush_done_cb(esp_lcd_panel_io_handle_t panel_io,
                              esp_lcd_panel_io_event_data_t *edata,
                              void *user_ctx) {
    (void)panel_io;
    (void)edata;
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(sem, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static esp_err_t wait_flush_done(void) {
    if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "LCD transfer timed out");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t draw_bitmap_blocking(int x, int y, int w, int h,
                                      const uint16_t *pixels) {
    if (!s_panel || !pixels || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h,
                                              pixels);
    if (err != ESP_OK) {
        return err;
    }
    return wait_flush_done();
}

static void fill_draw_buffer(uint16_t color, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i) {
        s_draw_buf[i] = color;
    }
}

static esp_err_t fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (!s_draw_buf || w <= 0 || h <= 0) {
        return ESP_OK;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > LCD_H_RES) {
        w = LCD_H_RES - x;
    }
    if (y + h > LCD_V_RES) {
        h = LCD_V_RES - y;
    }
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    int rows_per_chunk = LCD_DRAW_PIXELS / w;
    if (rows_per_chunk <= 0) {
        rows_per_chunk = 1;
    }
    while (h > 0) {
        int rows = h > rows_per_chunk ? rows_per_chunk : h;
        size_t pixels = (size_t)w * rows;
        fill_draw_buffer(color, pixels);
        ESP_RETURN_ON_ERROR(draw_bitmap_blocking(x, y, w, rows, s_draw_buf),
                            TAG, "draw rect failed");
        y += rows;
        h -= rows;
    }
    return ESP_OK;
}

static void glyph_for(char c, uint8_t out[5]) {
    memset(out, 0, 5);
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    switch (c) {
        case '0': memcpy(out, (uint8_t[]){0x3E, 0x51, 0x49, 0x45, 0x3E}, 5); break;
        case '1': memcpy(out, (uint8_t[]){0x00, 0x42, 0x7F, 0x40, 0x00}, 5); break;
        case '2': memcpy(out, (uint8_t[]){0x42, 0x61, 0x51, 0x49, 0x46}, 5); break;
        case '3': memcpy(out, (uint8_t[]){0x21, 0x41, 0x45, 0x4B, 0x31}, 5); break;
        case '4': memcpy(out, (uint8_t[]){0x18, 0x14, 0x12, 0x7F, 0x10}, 5); break;
        case '5': memcpy(out, (uint8_t[]){0x27, 0x45, 0x45, 0x45, 0x39}, 5); break;
        case '6': memcpy(out, (uint8_t[]){0x3C, 0x4A, 0x49, 0x49, 0x30}, 5); break;
        case '7': memcpy(out, (uint8_t[]){0x01, 0x71, 0x09, 0x05, 0x03}, 5); break;
        case '8': memcpy(out, (uint8_t[]){0x36, 0x49, 0x49, 0x49, 0x36}, 5); break;
        case '9': memcpy(out, (uint8_t[]){0x06, 0x49, 0x49, 0x29, 0x1E}, 5); break;
        case 'A': memcpy(out, (uint8_t[]){0x7E, 0x11, 0x11, 0x11, 0x7E}, 5); break;
        case 'B': memcpy(out, (uint8_t[]){0x7F, 0x49, 0x49, 0x49, 0x36}, 5); break;
        case 'C': memcpy(out, (uint8_t[]){0x3E, 0x41, 0x41, 0x41, 0x22}, 5); break;
        case 'D': memcpy(out, (uint8_t[]){0x7F, 0x41, 0x41, 0x22, 0x1C}, 5); break;
        case 'E': memcpy(out, (uint8_t[]){0x7F, 0x49, 0x49, 0x49, 0x41}, 5); break;
        case 'F': memcpy(out, (uint8_t[]){0x7F, 0x09, 0x09, 0x09, 0x01}, 5); break;
        case 'G': memcpy(out, (uint8_t[]){0x3E, 0x41, 0x49, 0x49, 0x7A}, 5); break;
        case 'H': memcpy(out, (uint8_t[]){0x7F, 0x08, 0x08, 0x08, 0x7F}, 5); break;
        case 'I': memcpy(out, (uint8_t[]){0x00, 0x41, 0x7F, 0x41, 0x00}, 5); break;
        case 'J': memcpy(out, (uint8_t[]){0x20, 0x40, 0x41, 0x3F, 0x01}, 5); break;
        case 'K': memcpy(out, (uint8_t[]){0x7F, 0x08, 0x14, 0x22, 0x41}, 5); break;
        case 'L': memcpy(out, (uint8_t[]){0x7F, 0x40, 0x40, 0x40, 0x40}, 5); break;
        case 'M': memcpy(out, (uint8_t[]){0x7F, 0x02, 0x0C, 0x02, 0x7F}, 5); break;
        case 'N': memcpy(out, (uint8_t[]){0x7F, 0x04, 0x08, 0x10, 0x7F}, 5); break;
        case 'O': memcpy(out, (uint8_t[]){0x3E, 0x41, 0x41, 0x41, 0x3E}, 5); break;
        case 'P': memcpy(out, (uint8_t[]){0x7F, 0x09, 0x09, 0x09, 0x06}, 5); break;
        case 'Q': memcpy(out, (uint8_t[]){0x3E, 0x41, 0x51, 0x21, 0x5E}, 5); break;
        case 'R': memcpy(out, (uint8_t[]){0x7F, 0x09, 0x19, 0x29, 0x46}, 5); break;
        case 'S': memcpy(out, (uint8_t[]){0x46, 0x49, 0x49, 0x49, 0x31}, 5); break;
        case 'T': memcpy(out, (uint8_t[]){0x01, 0x01, 0x7F, 0x01, 0x01}, 5); break;
        case 'U': memcpy(out, (uint8_t[]){0x3F, 0x40, 0x40, 0x40, 0x3F}, 5); break;
        case 'V': memcpy(out, (uint8_t[]){0x1F, 0x20, 0x40, 0x20, 0x1F}, 5); break;
        case 'W': memcpy(out, (uint8_t[]){0x3F, 0x40, 0x38, 0x40, 0x3F}, 5); break;
        case 'X': memcpy(out, (uint8_t[]){0x63, 0x14, 0x08, 0x14, 0x63}, 5); break;
        case 'Y': memcpy(out, (uint8_t[]){0x07, 0x08, 0x70, 0x08, 0x07}, 5); break;
        case 'Z': memcpy(out, (uint8_t[]){0x61, 0x51, 0x49, 0x45, 0x43}, 5); break;
        case '.': memcpy(out, (uint8_t[]){0x00, 0x60, 0x60, 0x00, 0x00}, 5); break;
        case ':': memcpy(out, (uint8_t[]){0x00, 0x36, 0x36, 0x00, 0x00}, 5); break;
        case '-': memcpy(out, (uint8_t[]){0x08, 0x08, 0x08, 0x08, 0x08}, 5); break;
        case '/': memcpy(out, (uint8_t[]){0x20, 0x10, 0x08, 0x04, 0x02}, 5); break;
        case '%': memcpy(out, (uint8_t[]){0x23, 0x13, 0x08, 0x64, 0x62}, 5); break;
        case '+': memcpy(out, (uint8_t[]){0x08, 0x08, 0x3E, 0x08, 0x08}, 5); break;
        case ' ': default: break;
    }
}

static esp_err_t draw_char(int x, int y, char c, int scale, uint16_t fg,
                           uint16_t bg) {
    if (scale <= 0) {
        return ESP_OK;
    }
    const int char_w = 6 * scale;
    const int char_h = 8 * scale;
    if (x >= LCD_H_RES || y >= LCD_V_RES || x + char_w <= 0 || y + char_h <= 0) {
        return ESP_OK;
    }
    if ((size_t)char_w * char_h > LCD_DRAW_PIXELS) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t glyph[5];
    glyph_for(c, glyph);
    fill_draw_buffer(bg, (size_t)char_w * char_h);

    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if ((glyph[col] & (1U << row)) == 0) {
                continue;
            }
            for (int sy = 0; sy < scale; ++sy) {
                int py = (row * scale) + sy;
                for (int sx = 0; sx < scale; ++sx) {
                    int px = (col * scale) + sx;
                    s_draw_buf[(py * char_w) + px] = fg;
                }
            }
        }
    }
    return draw_bitmap_blocking(x, y, char_w, char_h, s_draw_buf);
}

static esp_err_t draw_text(int x, int y, const char *text, int scale,
                           uint16_t fg, uint16_t bg) {
    if (!text) {
        return ESP_OK;
    }
    int cursor = x;
    const int char_w = 6 * scale;
    while (*text && cursor + char_w <= LCD_H_RES) {
        ESP_RETURN_ON_ERROR(draw_char(cursor, y, *text, scale, fg, bg),
                            TAG, "draw char failed");
        cursor += char_w;
        ++text;
    }
    return ESP_OK;
}

static esp_err_t draw_frame(void) {
    ESP_RETURN_ON_ERROR(fill_rect(0, 0, LCD_H_RES, 1, COLOR_LINE), TAG, "frame");
    ESP_RETURN_ON_ERROR(fill_rect(0, LCD_V_RES - 1, LCD_H_RES, 1, COLOR_LINE),
                        TAG, "frame");
    ESP_RETURN_ON_ERROR(fill_rect(0, 0, 1, LCD_V_RES, COLOR_LINE), TAG, "frame");
    ESP_RETURN_ON_ERROR(fill_rect(LCD_H_RES - 1, 0, 1, LCD_V_RES, COLOR_LINE),
                        TAG, "frame");
    return ESP_OK;
}

static void format_float_value(char *out, size_t size, bool has_value,
                               float value) {
    if (!out || size == 0) {
        return;
    }
    if (!has_value) {
        strlcpy(out, "--", size);
        return;
    }
    snprintf(out, size, "%.1f", value);
}

static void format_uint_value(char *out, size_t size, bool has_value,
                              unsigned value) {
    if (!out || size == 0) {
        return;
    }
    if (!has_value) {
        strlcpy(out, "--", size);
        return;
    }
    snprintf(out, size, "%u", value);
}

static air_quality_t air_quality_for_pm25(bool has_pm25, float pm25) {
    if (!has_pm25) {
        return (air_quality_t){
            .label = "WAIT",
            .bg = COLOR_PANEL_2,
            .fg = COLOR_MUTED,
        };
    }
    if (pm25 <= 12.0f) {
        return (air_quality_t){
            .label = "GOOD",
            .bg = COLOR_GREEN,
            .fg = COLOR_BLACK,
        };
    }
    if (pm25 <= 35.4f) {
        return (air_quality_t){
            .label = "OK",
            .bg = COLOR_YELLOW,
            .fg = COLOR_BLACK,
        };
    }
    if (pm25 <= 55.4f) {
        return (air_quality_t){
            .label = "POOR",
            .bg = COLOR_RED,
            .fg = COLOR_WHITE,
        };
    }
    return (air_quality_t){
        .label = "BAD",
        .bg = COLOR_MAGENTA,
        .fg = COLOR_WHITE,
    };
}

static esp_err_t draw_badge(int x, int y, int w, const char *label,
                            uint16_t bg, uint16_t fg) {
    ESP_RETURN_ON_ERROR(fill_rect(x, y, w, 16, bg), TAG, "badge");
    int text_w = (int)strlen(label) * 6;
    int text_x = x + ((w - text_w) / 2);
    if (text_x < x + 2) {
        text_x = x + 2;
    }
    return draw_text(text_x, y + 4, label, 1, fg, bg);
}

static esp_err_t draw_metric_cell(int x, int y, int w, int h,
                                  const char *label, const char *value,
                                  uint16_t value_color) {
    ESP_RETURN_ON_ERROR(fill_rect(x, y, w, h, COLOR_PANEL_2), TAG, "metric");
    ESP_RETURN_ON_ERROR(fill_rect(x, y, w, 1, COLOR_LINE), TAG, "metric line");
    ESP_RETURN_ON_ERROR(draw_text(x + 5, y + 3, label, 1, COLOR_MUTED,
                                  COLOR_PANEL_2),
                        TAG, "metric label");
    return draw_text(x + 5, y + 11, value, 2, value_color, COLOR_PANEL_2);
}

static esp_err_t draw_status_screen(void) {
    char value[16];
    char brightness[8];
    bool wifi_connected = station_is_connected();
    sensor_sample_t latest = {0};
    bool has_latest = sensor_service_get_latest(&latest);
    bool has_pm = has_latest && latest.pm_count > 0;
    tailnet_status_t tailnet = {0};
    tailnet_service_get_status(&tailnet);
    time_service_snapshot_t time_snapshot = {0};
    bool time_synced =
        time_service_get_snapshot(&time_snapshot) && time_snapshot.synced;
    air_quality_t quality = air_quality_for_pm25(has_pm, latest.pm2_5);

    ESP_RETURN_ON_ERROR(fill_rect(0, 0, LCD_H_RES, LCD_V_RES, COLOR_BLACK),
                        TAG, "clear");
    ESP_RETURN_ON_ERROR(draw_frame(), TAG, "frame");

    ESP_RETURN_ON_ERROR(fill_rect(2, 2, LCD_H_RES - 4, 28, COLOR_PANEL),
                        TAG, "header");
    ESP_RETURN_ON_ERROR(fill_rect(2, 30, LCD_H_RES - 4, 1, COLOR_LINE),
                        TAG, "header line");
    ESP_RETURN_ON_ERROR(draw_text(10, 8, "PLAIN AIR", 2, COLOR_TEXT,
                                  COLOR_PANEL),
                        TAG, "title");

    ESP_RETURN_ON_ERROR(draw_badge(174, 7, 18, "W",
                                  wifi_connected ? COLOR_GREEN : COLOR_LINE,
                                  wifi_connected ? COLOR_BLACK : COLOR_MUTED),
                        TAG, "wifi badge");
    ESP_RETURN_ON_ERROR(draw_badge(198, 7, 18, "V",
                                  tailnet.connected ? COLOR_GREEN : COLOR_LINE,
                                  tailnet.connected ? COLOR_BLACK : COLOR_MUTED),
                        TAG, "tailnet badge");
    ESP_RETURN_ON_ERROR(draw_badge(222, 7, 18, "T",
                                  time_synced ? COLOR_GREEN : COLOR_LINE,
                                  time_synced ? COLOR_BLACK : COLOR_MUTED),
                        TAG, "time badge");
    snprintf(brightness, sizeof(brightness), "B%u",
             s_brightness_percents[s_brightness_index]);
    ESP_RETURN_ON_ERROR(draw_badge(248, 7, 54, brightness,
                                  s_brightness_index == 0 ? COLOR_LINE
                                                          : COLOR_BLUE,
                                  s_brightness_index == 0 ? COLOR_MUTED
                                                          : COLOR_WHITE),
                        TAG, "brightness badge");

    ESP_RETURN_ON_ERROR(fill_rect(8, 38, 178, 70, COLOR_PANEL), TAG, "pm panel");
    ESP_RETURN_ON_ERROR(fill_rect(8, 38, 3, 70, quality.bg), TAG, "pm accent");
    ESP_RETURN_ON_ERROR(draw_text(18, 43, "PM2.5", 2, COLOR_MUTED,
                                  COLOR_PANEL),
                        TAG, "pm label");
    format_float_value(value, sizeof(value), has_pm, latest.pm2_5);
    ESP_RETURN_ON_ERROR(draw_text(18, 66, value, 5,
                                  has_pm ? quality.bg : COLOR_MUTED,
                                  COLOR_PANEL),
                        TAG, "pm value");

    ESP_RETURN_ON_ERROR(fill_rect(198, 38, 114, 70, quality.bg), TAG, "quality");
    ESP_RETURN_ON_ERROR(draw_text(213, 48, "AIR", 1, quality.fg, quality.bg),
                        TAG, "air label");
    int quality_w = (int)strlen(quality.label) * 18;
    int quality_x = 198 + ((114 - quality_w) / 2);
    ESP_RETURN_ON_ERROR(draw_text(quality_x, 64, quality.label, 3, quality.fg,
                                  quality.bg),
                        TAG, "quality label");

    const int box_w = 98;
    const int box_h = 27;
    const int gap = 5;
    const int x0 = 8;
    const int y0 = 112;
    const int y1 = 141;
    int x1 = x0 + box_w + gap;
    int x2 = x1 + box_w + gap;

    format_float_value(value, sizeof(value), has_pm, latest.pm10_0);
    ESP_RETURN_ON_ERROR(draw_metric_cell(x0, y0, box_w, box_h, "PM10", value,
                                         has_pm ? COLOR_TEXT : COLOR_MUTED),
                        TAG, "pm10 cell");

    format_float_value(value, sizeof(value),
                       has_latest && latest.has_temperature,
                       latest.temperature_c);
    ESP_RETURN_ON_ERROR(draw_metric_cell(x1, y0, box_w, box_h, "TEMP", value,
                                         has_latest && latest.has_temperature
                                             ? COLOR_TEXT
                                             : COLOR_MUTED),
                        TAG, "temp cell");

    format_float_value(value, sizeof(value), has_latest && latest.has_humidity,
                       latest.humidity_percent);
    ESP_RETURN_ON_ERROR(draw_metric_cell(x2, y0, box_w, box_h, "RH", value,
                                         has_latest && latest.has_humidity
                                             ? COLOR_TEXT
                                             : COLOR_MUTED),
                        TAG, "rh cell");

    format_uint_value(value, sizeof(value), has_latest && latest.has_co2,
                      latest.co2_ppm);
    ESP_RETURN_ON_ERROR(draw_metric_cell(x0, y1, box_w, box_h, "CO2", value,
                                         has_latest && latest.has_co2
                                             ? COLOR_TEXT
                                             : COLOR_MUTED),
                        TAG, "co2 cell");

    format_float_value(value, sizeof(value),
                       has_latest && latest.has_voc_index, latest.voc_index);
    ESP_RETURN_ON_ERROR(draw_metric_cell(x1, y1, box_w, box_h, "VOC", value,
                                         has_latest && latest.has_voc_index
                                             ? COLOR_TEXT
                                             : COLOR_MUTED),
                        TAG, "voc cell");

    format_float_value(value, sizeof(value),
                       has_latest && latest.has_nox_index, latest.nox_index);
    ESP_RETURN_ON_ERROR(draw_metric_cell(x2, y1, box_w, box_h, "NOX", value,
                                         has_latest && latest.has_nox_index
                                             ? COLOR_TEXT
                                             : COLOR_MUTED),
                        TAG, "nox cell");

    ++s_frame_count;
    return ESP_OK;
}

static esp_err_t send_panel_init_commands(void) {
    for (size_t i = 0; i < sizeof(lcd_st7789v_init) / sizeof(lcd_st7789v_init[0]);
         ++i) {
        const lcd_cmd_t *cmd = &lcd_st7789v_init[i];
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, cmd->cmd, cmd->data,
                                                      cmd->len & 0x7F),
                            TAG, "LCD init command failed");
        if (cmd->len & 0x80) {
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    }
    return ESP_OK;
}

static void backlight_set(uint8_t value) {
    const uint8_t steps = 16;
    if (value > steps) {
        value = steps;
    }
    if (value == 0) {
        gpio_set_level(PIN_LCD_BL, 0);
        vTaskDelay(pdMS_TO_TICKS(3));
        s_backlight_level = 0;
        return;
    }
    if (s_backlight_level == 0) {
        gpio_set_level(PIN_LCD_BL, 1);
        s_backlight_level = steps;
        esp_rom_delay_us(30);
    }
    int from = steps - s_backlight_level;
    int to = steps - value;
    int pulses = (steps + to - from) % steps;
    for (int i = 0; i < pulses; ++i) {
        gpio_set_level(PIN_LCD_BL, 0);
        gpio_set_level(PIN_LCD_BL, 1);
    }
    s_backlight_level = value;
}

static void backlight_apply_current(void) {
    backlight_set(s_brightness_levels[s_brightness_index]);
    ESP_LOGI(TAG, "display brightness: %u%%",
             s_brightness_percents[s_brightness_index]);
}

static void backlight_cycle(void) {
    s_brightness_index++;
    if (s_brightness_index >=
        sizeof(s_brightness_levels) / sizeof(s_brightness_levels[0])) {
        s_brightness_index = 0;
    }
    backlight_apply_current();
}

static esp_err_t init_button(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_BUTTON_2,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "button gpio config failed");

    s_button_last_raw_pressed = gpio_get_level(PIN_BUTTON_2) == 0;
    s_button_stable_pressed = s_button_last_raw_pressed;
    s_button_last_change_ms = esp_timer_get_time() / 1000LL;
    return ESP_OK;
}

static bool poll_button(void) {
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    bool raw_pressed = gpio_get_level(PIN_BUTTON_2) == 0;

    if (raw_pressed != s_button_last_raw_pressed) {
        s_button_last_raw_pressed = raw_pressed;
        s_button_last_change_ms = now_ms;
    }

    if ((now_ms - s_button_last_change_ms) < BUTTON_DEBOUNCE_MS ||
        raw_pressed == s_button_stable_pressed) {
        return false;
    }

    s_button_stable_pressed = raw_pressed;
    if (!s_button_stable_pressed) {
        return false;
    }

    backlight_cycle();
    return true;
}

static esp_err_t init_lcd(void) {
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << PIN_LCD_RD) | (1ULL << PIN_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "gpio config failed");
    gpio_set_level(PIN_LCD_RD, 1);
    gpio_set_level(PIN_LCD_BL, 0);
    ESP_RETURN_ON_ERROR(init_button(), TAG, "button init failed");

    s_flush_done = xSemaphoreCreateBinary();
    if (!s_flush_done) {
        return ESP_ERR_NO_MEM;
    }

    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_config = {
        .dc_gpio_num = PIN_LCD_DC,
        .wr_gpio_num = PIN_LCD_WR,
        .clk_src = LCD_CLK_SRC_PLL160M,
        .data_gpio_nums = {
            PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
            PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7,
        },
        .bus_width = 8,
        .max_transfer_bytes = LCD_DRAW_BUF_BYTES,
        .dma_burst_size = 64,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus_config, &i80_bus),
                        TAG, "create i80 bus failed");

    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 1,
        .on_color_trans_done = lcd_flush_done_cb,
        .user_ctx = s_flush_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .flags = {
            .swap_color_bytes = 1,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &s_io),
                        TAG, "create i80 panel io failed");

    s_draw_buf = esp_lcd_i80_alloc_draw_buffer(s_io, LCD_DRAW_BUF_BYTES,
                                               MALLOC_CAP_DMA |
                                                   MALLOC_CAP_INTERNAL);
    if (!s_draw_buf) {
        ESP_LOGE(TAG, "failed to allocate LCD draw buffer");
        return ESP_ERR_NO_MEM;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RES,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel),
                        TAG, "create st7789 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset LCD failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init LCD failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true),
                        TAG, "invert LCD failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true),
                        TAG, "swap LCD axes failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, false, true),
                        TAG, "mirror LCD failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, 35),
                        TAG, "set LCD gap failed");
    ESP_RETURN_ON_ERROR(send_panel_init_commands(), TAG, "LCD init sequence failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true),
                        TAG, "turn LCD on failed");
    return ESP_OK;
}

static void display_task(void *arg) {
    (void)arg;
    int64_t last_draw_ms = esp_timer_get_time() / 1000LL;

    while (1) {
        bool brightness_changed = poll_button();
        int64_t now_ms = esp_timer_get_time() / 1000LL;
        bool refresh_due = (now_ms - last_draw_ms) >= DISPLAY_REFRESH_MS;

        if (brightness_changed && s_brightness_levels[s_brightness_index] > 0) {
            refresh_due = true;
        }

        if (refresh_due) {
            esp_err_t err = draw_status_screen();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "display update failed: %s", esp_err_to_name(err));
            } else {
                last_draw_ms = now_ms;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t display_service_start(const display_service_config_t *config) {
    if (s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config && config->device_name && config->device_name[0]) {
        strlcpy(s_device_name, config->device_name, sizeof(s_device_name));
    }

    ESP_RETURN_ON_ERROR(init_lcd(), TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(draw_status_screen(), TAG, "initial screen failed");
    backlight_apply_current();

    BaseType_t ok = xTaskCreatePinnedToCore(display_task, "display", 4096, NULL,
                                           3, &s_task, 1);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "display started for %s", s_device_name);
    return ESP_OK;
}
