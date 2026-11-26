/* bys_demo: listens for bysound v4 messages (and acknowledges them); the BOOT button sends a text and waits for the ack. */
#include "audio.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "bys_demo";

static void on_text(const char *text) { ESP_LOGI(TAG, "received: %s", text); }

void app_main(void) {
  ESP_ERROR_CHECK(audio_start(on_text));
  ESP_LOGI(TAG, "listening; press the button (GPIO%d) to send \"%s\"", CONFIG_BYSOUND_DEMO_BUTTON_GPIO, CONFIG_BYSOUND_DEMO_TEXT);

  const gpio_config_t btn = { .pin_bit_mask = 1ULL << CONFIG_BYSOUND_DEMO_BUTTON_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
  ESP_ERROR_CHECK(gpio_config(&btn));

  int was_down = 0;
  for (;;) {
    int down = gpio_get_level(CONFIG_BYSOUND_DEMO_BUTTON_GPIO) == 0;
    if (down && !was_down && audio_send(CONFIG_BYSOUND_DEMO_TEXT) != ESP_OK) ESP_LOGW(TAG, "busy sending");
    was_down = down;
    vTaskDelay(pdMS_TO_TICKS(30)); /* the poll interval doubles as debounce */
  }
}
