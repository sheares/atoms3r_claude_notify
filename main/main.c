#include "gc9107.h"
#include "lp5562.h"
#include "wifi.h"
#include "bmi270.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

static const char *TAG = "notify";

#define BTN_GPIO  41

// ─── App state ────────────────────────────────────────────────────────────────
typedef enum { APP_STANDBY, APP_THINKING, APP_DONE, APP_WAITING } app_state_t;
static volatile app_state_t s_app_state  = APP_STANDBY;
static volatile int64_t     s_notify_us  = 0;
static char                 s_device_ip[20] = "connecting...";
static bool                 s_wifi_ok    = false;
static bool                 s_imu_ok     = false;

// ─── Colors ───────────────────────────────────────────────────────────────────
#define C_ORANGE    RGB565(245, 110, 45)   // standby + thinking bg
#define C_EYE       RGB565( 20,  15,  10)
#define C_WHITE     RGB565(255, 255, 255)
#define C_DIM       RGB565(170,  65,  20)
#define C_FAINT     RGB565(235, 175, 150)

#define C_DONE_BG   RGB565( 30, 160,  30)  // green for done screen
#define C_DONE_DIM  RGB565( 20, 110,  20)
#define C_CHECK     RGB565(255, 255, 255)
#define C_TICK      RGB565( 60, 220,  60)  // green tick

#define C_ERR_BG    RGB565(160,  20,  20)

// ─── Pixel art eye bitmaps ────────────────────────────────────────────────────
#define PS       5
#define EYE_COLS 4

static const uint8_t BMP_OPEN[4][4] = {
    {1,1,1,1}, {1,1,1,1}, {1,1,1,1}, {1,1,1,1},
};
static const uint8_t BMP_HALF[2][4] = {
    {1,1,1,1}, {1,1,1,1},
};
static const uint8_t BMP_BLINK[1][4] = {
    {1,1,1,1},
};
static const uint8_t BMP_HAPPY[3][4] = {
    {0,1,1,0}, {1,1,1,1}, {1,0,0,1},  // thick open arch: ^ shape
};
static const uint8_t BMP_CHEVRON_R[5][4] = {  // ">" outline — left eye on waiting screen
    {1,1,0,0}, {0,1,1,0}, {0,0,1,1}, {0,1,1,0}, {1,1,0,0},
};
static const uint8_t BMP_CHEVRON_L[5][4] = {  // "<" outline — right eye on waiting screen
    {0,0,1,1}, {0,1,1,0}, {1,1,0,0}, {0,1,1,0}, {0,0,1,1},
};

typedef enum { EYE_OPEN, EYE_HALF, EYE_BLINK, EYE_HAPPY } eye_state_t;

// ─── Eye layout ───────────────────────────────────────────────────────────────
#define EYE_L_X  24
#define EYE_R_X 104
#define EYE_Y    48

// ─── Blink state machine ──────────────────────────────────────────────────────
static int s_blink_timer = 90;
static int s_blink_phase = 0;

// ─── Wander (shared between standby and done) ─────────────────────────────────
static float s_eye_ox       = 0.0f;
static float s_eye_target_x = 0.0f;
static int   s_wander_timer = 60;

// ─── Thinking motion ─────────────────────────────────────────────────────────
static int   s_think_tick   = 0;
static float s_eye_oy       = 0.0f;

// ─── Drawing helpers ──────────────────────────────────────────────────────────

static void draw_centred(int y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale)
{
    int w = (int)strlen(s) * 6 * scale;
    int x = (LCD_WIDTH - w) / 2;
    gc9107_draw_string(x, y, s, fg, bg, scale);
}

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

static void draw_pixel_eye(int cx, int cy, const uint8_t *bmp, int rows)
{
    int ox = cx - (EYE_COLS * PS) / 2;
    int oy = cy - (rows * PS) / 2;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < EYE_COLS; c++)
            if (bmp[r * EYE_COLS + c])
                gc9107_fill_rect(ox + c * PS, oy + r * PS, PS, PS, C_EYE);
}

static void draw_eyes(int lx, int rx, int y, eye_state_t state)
{
    const uint8_t *bmp;
    int rows;
    switch (state) {
    case EYE_OPEN:  bmp = &BMP_OPEN[0][0];  rows = 4; break;
    case EYE_HALF:  bmp = &BMP_HALF[0][0];  rows = 2; break;
    case EYE_BLINK: bmp = &BMP_BLINK[0][0]; rows = 1; break;
    case EYE_HAPPY: bmp = &BMP_HAPPY[0][0]; rows = 3; break;
    default:        bmp = &BMP_OPEN[0][0];  rows = 4; break;
    }
    draw_pixel_eye(lx, y, bmp, rows);
    draw_pixel_eye(rx, y, bmp, rows);
}

// ─── Animation updates ────────────────────────────────────────────────────────

static void update_blink(void)
{
    if (s_blink_phase == 0) {
        if (--s_blink_timer <= 0) {
            s_blink_phase = 1;
            s_blink_timer = 2;
        }
    } else {
        if (--s_blink_timer <= 0) {
            if (++s_blink_phase > 3) {
                s_blink_phase = 0;
                s_blink_timer = 80 + rand() % 60;
            } else {
                s_blink_timer = 2;
            }
        }
    }
}

static eye_state_t blink_eye_state(void)
{
    switch (s_blink_phase) {
    case 1: return EYE_HALF;
    case 2: return EYE_BLINK;
    case 3: return EYE_HALF;
    default: return EYE_OPEN;
    }
}

static void update_wander(void)
{
    if (--s_wander_timer <= 0) {
        int choices[] = {-8, -4, 0, 0, 0, 4, 8};
        s_eye_target_x = (float)choices[rand() % 7];
        s_wander_timer = 40 + rand() % 80;
    }
    s_eye_ox += (s_eye_target_x - s_eye_ox) * 0.04f;
}

static void update_thinking_motion(void)
{
    s_think_tick++;
    // Faster, larger horizontal sweeps — always displaced, never centered long
    if (--s_wander_timer <= 0) {
        int choices[] = {-11, -7, -4, 4, 7, 11};
        s_eye_target_x = (float)choices[rand() % 6];
        s_wander_timer = 8 + rand() % 14;
    }
    s_eye_ox += (s_eye_target_x - s_eye_ox) * 0.18f;
    // Vertical bob using a sine wave
    s_eye_oy = 3.0f * sinf(s_think_tick * 0.14f);
}

// ─── Screen renderers ─────────────────────────────────────────────────────────

static void draw_standby(void)
{
    gc9107_fill_screen(C_ORANGE);
    draw_centred(6, "CLAUDE CODE", C_FAINT, C_ORANGE, 1);
    int ox = (int)s_eye_ox;
    draw_eyes(EYE_L_X + ox, EYE_R_X + ox, EYE_Y, blink_eye_state());
    if (s_wifi_ok) {
        draw_centred(104, s_device_ip,       C_FAINT, C_ORANGE, 1);
        draw_centred(116, "curl <ip>/notify", C_DIM,   C_ORANGE, 1);
    } else {
        draw_centred(110, "connecting...", C_DIM, C_ORANGE, 1);
    }
}

static void draw_thinking(void)
{
    gc9107_fill_screen(C_ORANGE);

    // Animated "..." — cycles every ~10 frames (~330 ms)
    static int s_dot_frame = 0;
    s_dot_frame++;
    static const char *dot_frames[] = { ".  ", ".. ", "...", " ..", "  .", "   " };
    const char *dots = dot_frames[(s_dot_frame / 8) % 6];
    draw_centred(8, dots, C_DIM, C_ORANGE, 2);

    int ox = (int)s_eye_ox;
    int oy = (int)s_eye_oy;
    draw_eyes(EYE_L_X + ox, EYE_R_X + ox, EYE_Y + oy, blink_eye_state());

    draw_centred(108, "thinking...", C_DIM, C_ORANGE, 1);
}

static void draw_done(void)
{
    gc9107_fill_screen(C_DONE_BG);

    draw_centred(8, "DONE!", C_CHECK, C_DONE_BG, 1);

    // Happy eyes — at y=42, wider apart (L=24, R=104)
    // rows=2, PS=5: eye spans y=37–47. Checkmark starts at y=62 → clear gap.
    int ox = (int)s_eye_ox;
    draw_eyes(EYE_L_X + ox, EYE_R_X + ox, 42, EYE_HAPPY);

    // Checkmark — green, thick
    draw_thick_line(45, 83, 57, 95, 5, C_TICK);   // short left leg
    draw_thick_line(57, 95, 86, 66, 5, C_TICK);   // long right leg

    // Elapsed time
    int64_t secs = (esp_timer_get_time() - s_notify_us) / 1000000LL;
    char elapsed[20];
    if (secs < 5)       snprintf(elapsed, sizeof(elapsed), "just now");
    else if (secs < 60) snprintf(elapsed, sizeof(elapsed), "%llds ago", (long long)secs);
    else                snprintf(elapsed, sizeof(elapsed), "%lldm ago", (long long)(secs / 60));
    draw_centred(110, elapsed, C_DONE_DIM, C_DONE_BG, 1);
}

static void draw_waiting(void)
{
    gc9107_fill_screen(C_ORANGE);
    draw_centred(5, "?", C_DIM, C_ORANGE, 3);

    int ox = (int)s_eye_ox;
    // Asymmetric "> <" chevron eyes — fixed shape, no blinking
    draw_pixel_eye(EYE_L_X + ox, EYE_Y, &BMP_CHEVRON_R[0][0], 5);
    draw_pixel_eye(EYE_R_X + ox, EYE_Y, &BMP_CHEVRON_L[0][0], 5);

    draw_centred(106, "your input?", C_DIM, C_ORANGE, 1);
}

static void draw_connecting(void)
{
    gc9107_fill_screen(C_ORANGE);
    draw_centred(44, "CLAUDE", C_WHITE, C_ORANGE, 2);
    draw_centred(62, "CODE",   C_WHITE, C_ORANGE, 2);
    draw_centred(94, "connecting...", C_DIM, C_ORANGE, 1);
    gc9107_flush();
}

static void draw_wifi_error(void)
{
    gc9107_fill_screen(C_ERR_BG);
    draw_centred(44, "WiFi",            C_WHITE, C_ERR_BG, 2);
    draw_centred(62, "FAILED",          C_WHITE, C_ERR_BG, 2);
    draw_centred(90, "check secrets.h", C_WHITE, C_ERR_BG, 1);
    gc9107_flush();
}

// ─── HTTP server ──────────────────────────────────────────────────────────────

static esp_err_t handle_notify(httpd_req_t *req)
{
    s_notify_us = esp_timer_get_time();
    s_app_state = APP_DONE;
    httpd_resp_sendstr(req, "OK\n");
    ESP_LOGI(TAG, "Done notification received");
    return ESP_OK;
}

static esp_err_t handle_thinking(httpd_req_t *req)
{
    s_think_tick   = 0;
    s_eye_oy       = 0.0f;
    s_wander_timer = 1;   // trigger a new target immediately
    s_app_state    = APP_THINKING;
    httpd_resp_sendstr(req, "OK\n");
    ESP_LOGI(TAG, "Thinking state activated");
    return ESP_OK;
}

static esp_err_t handle_clear(httpd_req_t *req)
{
    s_app_state = APP_STANDBY;
    httpd_resp_sendstr(req, "OK\n");
    return ESP_OK;
}

static esp_err_t handle_question(httpd_req_t *req)
{
    s_wander_timer = 1;
    s_app_state    = APP_WAITING;
    httpd_resp_sendstr(req, "OK\n");
    ESP_LOGI(TAG, "Waiting state activated");
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
    static const httpd_uri_t u_notify   = { .uri = "/notify",   .method = HTTP_GET, .handler = handle_notify   };
    static const httpd_uri_t u_thinking = { .uri = "/thinking", .method = HTTP_GET, .handler = handle_thinking };
    static const httpd_uri_t u_clear    = { .uri = "/clear",    .method = HTTP_GET, .handler = handle_clear    };
    static const httpd_uri_t u_question = { .uri = "/question", .method = HTTP_GET, .handler = handle_question };
    httpd_register_uri_handler(srv, &u_notify);
    httpd_register_uri_handler(srv, &u_thinking);
    httpd_register_uri_handler(srv, &u_clear);
    httpd_register_uri_handler(srv, &u_question);
    ESP_LOGI(TAG, "HTTP server ready (/notify /thinking /clear /question)");
}

// ─── Orientation polling ──────────────────────────────────────────────────────
// Reads the accelerometer and updates screen rotation when the device is held in
// a stable new orientation for ~1 second (3 consecutive matching polls at 10-frame
// intervals). Device-flat (az dominant) is ignored — keep last known rotation.
//
// Axis-to-rotation mapping may need tuning depending on AtomS3R physical mount:
//   ay > 0 → 0° (USB at bottom, normal)
//   ay < 0 → 180° (USB at top)
//   ax > 0 → 90° CW
//   ax < 0 → 270° CW
static void poll_orientation(void)
{
    if (!s_imu_ok) return;

    int16_t ax, ay, az;
    if (!bmi270_read_accel(&ax, &ay, &az)) return;

    // Ignore if device is mostly flat (az dominant) — noise in ax/ay unreliable
    if (abs(az) > abs(ax) && abs(az) > abs(ay)) return;

    uint8_t new_rot;
    if (abs(ax) > abs(ay))
        new_rot = (ax > 0) ? 0 : 2;
    else
        new_rot = (ay > 0) ? 3 : 1;

    static uint8_t  s_pending      = 3;
    static int      s_stable_count = 0;
    static uint8_t  s_current_rot  = 3;

    if (new_rot == s_pending) {
        if (++s_stable_count >= 3 && new_rot != s_current_rot) {
            s_current_rot = new_rot;
            gc9107_set_rotation(new_rot);
            ESP_LOGI(TAG, "Orientation → %d°", new_rot * 90);
        }
    } else {
        s_pending      = new_rot;
        s_stable_count = 0;
    }
}

// ─── Entry point ──────────────────────────────────────────────────────────────
void app_main(void)
{
    gc9107_init();
    i2c_master_bus_handle_t sys_bus;
    if (lp5562_init(&sys_bus)) lp5562_set_backlight(sys_bus, 200);
    s_imu_ok = bmi270_init(sys_bus);
    if (!s_imu_ok) ESP_LOGW(TAG, "BMI270 not found — orientation fixed at 0°");

    draw_connecting();

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    s_wifi_ok = wifi_connect(s_device_ip, sizeof(s_device_ip));
    if (!s_wifi_ok) {
        draw_wifi_error();
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    start_server();
    srand((unsigned)esp_timer_get_time());
    s_blink_timer = 60 + rand() % 60;

    int  btn_debounce  = 0;
    bool btn_triggered = false;

    for (;;) {
        // Button: clear to standby
        if (gpio_get_level(BTN_GPIO) == 0) {
            if (btn_debounce < 3) btn_debounce++;
        } else {
            btn_debounce  = 0;
            btn_triggered = false;
        }
        if (btn_debounce == 3 && !btn_triggered) {
            s_app_state   = APP_STANDBY;
            btn_triggered = true;
        }

        // Auto-dismiss done screen after 5 minutes
        if (s_app_state == APP_DONE) {
            int64_t age = (esp_timer_get_time() - s_notify_us) / 1000000LL;
            if (age > 300) s_app_state = APP_STANDBY;
        }

        // Poll IMU for orientation every 10 frames (~300 ms)
        static int s_imu_tick = 0;
        if (++s_imu_tick >= 10) {
            s_imu_tick = 0;
            poll_orientation();
        }

        // Update animation
        if (s_app_state == APP_THINKING) {
            update_blink();
            update_thinking_motion();
        } else {
            update_blink();
            update_wander();
        }

        // Draw
        switch (s_app_state) {
        case APP_STANDBY:  draw_standby();  break;
        case APP_THINKING: draw_thinking(); break;
        case APP_DONE:     draw_done();     break;
        case APP_WAITING:  draw_waiting();  break;
        }
        gc9107_flush();

        vTaskDelay(pdMS_TO_TICKS(33));
    }
}
