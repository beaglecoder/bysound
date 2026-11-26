set(AUDIO_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../esp32/main)
idf_component_register(SRCS "main.c" "wifi_text.c" "${AUDIO_DIR}/audio.c"
                       INCLUDE_DIRS "." "${AUDIO_DIR}"
                       REQUIRES bysound driver esp_driver_gpio esp_driver_i2s esp_timer nvs_flash
                                esp_wifi esp_netif esp_event mbedtls esp_hw_support)
