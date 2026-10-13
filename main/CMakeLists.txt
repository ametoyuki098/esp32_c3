idf_component_register(
    SRCS "esp32_c3.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_server
        esp_driver_gpio
	esp_driver_ledc
	esp_driver_uart
)

