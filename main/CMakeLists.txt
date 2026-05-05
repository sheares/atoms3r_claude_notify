idf_component_register(
    SRCS "main.c" "wifi.c" "gc9107.c" "lp5562.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server nvs_flash esp_wifi esp_event esp_netif esp_driver_i2c esp_driver_spi esp_driver_gpio
    PRIV_REQUIRES esp_timer
)
