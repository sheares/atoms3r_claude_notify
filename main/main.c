#include "gc9107.h"
#include "lp5562.h"
#include "wifi.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

static const char *TAG = "notify";

// ─── Button ───────────────────────────────────────────────────────────────────
#define BTN_GPIO  41

// ─── Notification state (written by HTTP task, read by main task) ─────────────
static volatile bool    s_notified   = false;
static volatile int64_t s_notify_us  = 0;   // esp_timer_get_time() at notification
static char             s_device_ip[20] = "connecting...";
static bool             s_wifi_ok    = false;

// ─── Colours ─────────────────────────────────────────────────────────────────
// Standby — dark terminal palette
#define C_BG        RGB565(  6,  10,  18)
#define C_WHITE     RGB565(255, 255, 255)
#define C_ORANGE    RGB565(255, 145,   0)
#define C_GRAY      RGB565(120, 130, 148)
#define C_DIM       RGB565( 38,  45,  58)
#define C_DOT_ON    RGB565( 80, 160, 255)
#define C_DOT_OFF   RGB565( 22,  30,  44)

// Notification — green success palette
#define C_GRN_BG    RGB565( 22, 168,  72)
#define C_GRN_DARK  RGB565( 12,  90,  38)
#define C_GRN_MID   RGB565( 18, 130,  56)
#define C_CHECK     RGB565(255, 255, 255)

// Error
#define C_ERR_BG    RGB565(160,  20,  20)

// ─── Drawing helpers ──────────────────────────────────────────────────────────

// Bresenham line, drawn w pixels thick (square brush)
static void draw_thick_line(int x0, int y0, int x1, int y1, int w, uint16_t color)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, half = w / 2;
    while (1) {
        gc9107_fill_rect(x0 - half, y0 - half, w, w, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Centre a string (scale 1 = 6 px/char, scale 2 = 12 px/char)
static void draw_centred(int y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale)
{
    int w = (int)strlen(s) * (5 + 1) * scale;
    int x = (LCD_WIDTH - w) / 2;
    gc9107_draw_string(x, y, s, fg, bg, scale);
}

// ─── Screen renderers ─────────────────────────────────────────────────────────

static void draw_standby(int frame)
{
    gc9107_fill_screen(C_BG);

    // ── Title ──────────────────────────────────────────────────────────────────
    draw_centred( 8, "CLAUDE", C_WHITE,  C_BG, 2);
    draw_centred(26, "CODE",   C_ORANGE, C_BG, 2);

    // ── Separator ─────────────────────────────────────────────────────────────
    gc9107_draw_hline(14, 47, 100, C_DIM);

    // ── Animated three-dot waiting indicator ──────────────────────────────────
    // Dot cycles every 20 frames (~0.66 s each)
    int active = (frame / 20) % 3;
    int dot_y = 62;
    int dot_xs[3] = {38, 64, 90};
    for (int i = 0; i < 3; i++) {
        uint16_t c = (i == active) ? C_DOT_ON : C_DOT_OFF;
        gc9107_fill_circle(dot_xs[i], dot_y, 7, c);
    }

    // ── Separator ─────────────────────────────────────────────────────────────
    gc9107_draw_hline(14, 80, 100, C_DIM);

    // ── IP / hostname ─────────────────────────────────────────────────────────
    if (s_wifi_ok) {
        draw_centred(88,  s_device_ip,          C_GRAY, C_BG, 1);
        draw_centred(100, "claude-notify.local", C_DIM,  C_BG, 1);
    } else {
        draw_centred(88, "connecting...", C_DIM, C_BG, 1);
    }

    // ── Bottom hint ───────────────────────────────────────────────────────────
    draw_centred(114, "curl <ip>/notify", C_DIM, C_BG, 1);
}

static void draw_notified(void)
{
    gc9107_fill_screen(C_GRN_BG);

    // ── Large checkmark (two thick line segments) ─────────────────────────────
    // Left arm: (36,60) → (52,76)
    draw_thick_line(36, 60, 52, 76, 5, C_CHECK);
    // Right arm: (52,76) → (90,34)
    draw_thick_line(52, 76, 90, 34, 5, C_CHECK);

    // ── "DONE!" text ──────────────────────────────────────────────────────────
    draw_centred(86, "DONE!", C_CHECK, C_GRN_BG, 2);

    // ── Elapsed time ─────────────────────────────────────────────────────────
    int64_t secs = (esp_timer_get_time() - s_notify_us) / 1000000LL;
    char elapsed[20];
    if (secs < 5)        snprintf(elapsed, sizeof(elapsed), "just now");
    else if (secs < 60)  snprintf(elapsed, sizeof(elapsed), "%llds ago", (long long)secs);
    else                 snprintf(elapsed, sizeof(elapsed), "%lldm ago", (long long)(secs / 60));
    draw_centred(106, elapsed, C_GRN_DARK, C_GRN_BG, 1);
}

static void draw_connecting(void)
{
    gc9107_fill_screen(C_BG);
    draw_centred(44, "CLAUDE", C_WHITE,  C_BG, 2);
    draw_centred(62, "CODE",   C_ORANGE, C_BG, 2);
    draw_centred(92, "connecting...", C_DIM, C_BG, 1);
    gc9107_flush();
}

static void draw_wifi_error(void)
{
    gc9107_fill_screen(C_ERR_BG);
    draw_centred(44, "WiFi",  C_WHITE, C_ERR_BG, 2);
    draw_centred(62, "FAILED", C_WHITE, C_ERR_BG, 2);
    draw_centred(90, "check secrets.h", C_WHITE, C_ERR_BG, 1);
    gc9107_flush();
}

// ─── HTTP server ──────────────────────────────────────────────────────────────

static esp_err_t handle_notify(httpd_req_t *req)
{
    s_notified  = true;
    s_notify_us = esp_timer_get_time();
    httpd_resp_sendstr(req, "OK\n");
    ESP_LOGI(TAG, "Notification received");
    return ESP_OK;
}

static esp_err_t handle_clear(httpd_req_t *req)
{
    s_notified = false;
    httpd_resp_sendstr(req, "OK\n");
    return ESP_OK;
}

static void start_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t srv;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t u_notify = { .uri = "/notify", .method = HTTP_GET,
                              .handler = handle_notify };
    httpd_uri_t u_clear  = { .uri = "/clear",  .method = HTTP_GET,
                              .handler = handle_clear  };
    httpd_register_uri_handler(srv, &u_notify);
    httpd_register_uri_handler(srv, &u_clear);
    ESP_LOGI(TAG, "HTTP server ready — GET /notify to trigger, GET /clear to dismiss");
}

// ─── Entry point ─────────────────────────────────────────────────────────────
void app_main(void)
{
    // Display + backlight
    gc9107_init();
    i2c_master_bus_handle_t sys_bus;
    if (lp5562_init(&sys_bus)) lp5562_set_backlight(sys_bus, 200);

    draw_connecting();

    // Button
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    // NVS (required by WiFi driver)
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // WiFi
    s_wifi_ok = wifi_connect(s_device_ip, sizeof(s_device_ip));
    if (!s_wifi_ok) {
        draw_wifi_error();
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // HTTP server
    start_server();

    // ── Main loop ─────────────────────────────────────────────────────────────
    int  frame         = 0;
    int  btn_debounce  = 0;
    bool btn_triggered = false;

    for (;;) {
        // Button: dismiss notification
        if (gpio_get_level(BTN_GPIO) == 0) {
            if (btn_debounce < 3) btn_debounce++;
        } else {
            btn_debounce  = 0;
            btn_triggered = false;
        }
        if (btn_debounce == 3 && !btn_triggered) {
            s_notified    = false;
            btn_triggered = true;
        }

        // Auto-dismiss after 5 minutes
        if (s_notified) {
            int64_t age = (esp_timer_get_time() - s_notify_us) / 1000000LL;
            if (age > 300) s_notified = false;
        }

        // Render
        if (s_notified) {
            draw_notified();
        } else {
            draw_standby(frame);
        }
        gc9107_flush();

        frame++;
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}
