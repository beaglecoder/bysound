idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES bysound driver esp_driver_gpio esp_driver_i2s esp_timer nvs_flash
                                esp_wifi esp_netif esp_event mbedtls esp_hw_support)
