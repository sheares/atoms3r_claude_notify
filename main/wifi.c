#include "wifi.h"
#include "secrets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

#define CONNECTED_BIT  BIT0
#define FAILED_BIT     BIT1
#define MAX_RETRIES    10

static const char *TAG = "wifi";
static EventGroupHandle_t s_eg;
static esp_netif_t       *s_netif;
static int                s_retries;

static void handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries < MAX_RETRIES) { esp_wifi_connect(); s_retries++; }
        else xEventGroupSetBits(s_eg, FAILED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retries = 0;
        xEventGroupSetBits(s_eg, CONNECTED_BIT);
    }
}

bool wifi_connect(char *ip_buf, size_t ip_len)
{
    s_eg    = xEventGroupCreate();
    s_netif = NULL;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h_wifi, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, handler, NULL, &h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, handler, NULL, &h_ip));

    wifi_config_t wcfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    // Copy credentials so struct initialiser works with macro strings
    strncpy((char *)wcfg.sta.ssid,     WIFI_SSID, sizeof(wcfg.sta.ssid)     - 1);
    strncpy((char *)wcfg.sta.password, WIFI_PASS, sizeof(wcfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_eg,
        CONNECTED_BIT | FAILED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));

    if (bits & CONNECTED_BIT) {
        esp_netif_ip_info_t info;
        esp_netif_get_ip_info(s_netif, &info);
        snprintf(ip_buf, ip_len, IPSTR, IP2STR(&info.ip));
        ESP_LOGI(TAG, "Connected — IP %s", ip_buf);
        return true;
    }
    ESP_LOGE(TAG, "WiFi connection failed after %d retries", MAX_RETRIES);
    return false;
}
