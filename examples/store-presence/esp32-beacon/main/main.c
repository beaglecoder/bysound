/* store-presence beacon: every CONFIG_PRESENCE_INTERVAL_S seconds it plays a short presence token on the `top` band with the plain modem.
 *
 *     P1|<storeId>|<window>|<mac>      window = floor(unix_time / 30)
 *                                      mac    = first 6 bytes (12 hex) of HMAC-SHA256(storeKey, "<storeId>|<window>")
 *
 * It is a broadcast: nothing is listened to, no session, no acknowledgement. The token is the same one examples/store-presence/token.ts makes.
 * Time: SNTP over Wi-Fi, or a fixed epoch plus uptime for an offline demo (see Kconfig). Speaker pins and level come from the esp32 demo's Kconfig. */
#include <stdio.h>
#include <string.h>

#include "bysound.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "sdkconfig.h"

#ifdef CONFIG_PRESENCE_TIME_SNTP
#include <time.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#endif

#define RATE   48000
#define BLOCK  480 /* frames per I2S transfer: 10 ms */
#define WINDOW_SECONDS 30

static const char *TAG = "presence";
static i2s_chan_handle_t s_tx;
static bys_tx_t s_state; /* static: it is large */
static int32_t s_buf32[BLOCK * 2];
static int16_t s_pcm[BLOCK];

/* ---------- time ---------- */
#ifdef CONFIG_PRESENCE_TIME_SNTP
static EventGroupHandle_t s_ev;
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg; (void)data;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
  else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) { ESP_LOGW(TAG, "wifi: disconnected, retrying"); esp_wifi_connect(); }
  else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_ev, BIT0);
}
static void time_init(void) {
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); ESP_ERROR_CHECK(nvs_flash_init()); }
  s_ev = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));
  wifi_config_t wc = { 0 };
  strlcpy((char *)wc.sta.ssid, CONFIG_PRESENCE_WIFI_SSID, sizeof wc.sta.ssid);
  strlcpy((char *)wc.sta.password, CONFIG_PRESENCE_WIFI_PASSWORD, sizeof wc.sta.password);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());
  xEventGroupWaitBits(s_ev, BIT0, pdFALSE, pdFALSE, portMAX_DELAY);
  esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_PRESENCE_SNTP_SERVER);
  ESP_ERROR_CHECK(esp_netif_sntp_init(&sc));
  while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(5000)) != ESP_OK) ESP_LOGW(TAG, "waiting for SNTP");
  ESP_LOGI(TAG, "time synchronised");
}
static int64_t unix_now(void) { return (int64_t)time(NULL); } /* re-synchronised by the SNTP client every hour by default */
#else
static void time_init(void) { ESP_LOGW(TAG, "offline demo: clock = %d + uptime. Set it to the time of flashing, or use SNTP.", CONFIG_PRESENCE_EPOCH); }
static int64_t unix_now(void) { return (int64_t)CONFIG_PRESENCE_EPOCH + esp_timer_get_time() / 1000000; }
#endif

/* ---------- token ---------- */
static void make_token(char *out, size_t cap, int64_t now) {
  const unsigned window = (unsigned)(now / WINDOW_SECONDS);
  char msg[48];
  const int n = snprintf(msg, sizeof msg, "%s|%u", CONFIG_PRESENCE_STORE_ID, window);
  uint8_t mac[32];
  const mbedtls_md_info_t *sha = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  ESP_ERROR_CHECK(mbedtls_md_hmac(sha, (const uint8_t *)CONFIG_PRESENCE_STORE_KEY, strlen(CONFIG_PRESENCE_STORE_KEY), (const uint8_t *)msg, (size_t)n, mac));
  snprintf(out, cap, "P1|%s|%u|%02x%02x%02x%02x%02x%02x", CONFIG_PRESENCE_STORE_ID, window, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---------- speaker (I2S, stereo 32-bit Philips at 48 kHz, MAX98357A mixes both slots) ---------- */
static void speaker_init(void) {
  i2s_std_config_t cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = CONFIG_BYSOUND_DEMO_SPK_BCLK, .ws = CONFIG_BYSOUND_DEMO_SPK_WS,
                  .dout = CONFIG_BYSOUND_DEMO_SPK_DIN, .din = I2S_GPIO_UNUSED },
  };
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_frame_num = BLOCK;
  cc.dma_desc_num = 6;
  cc.auto_clear = true;
  ESP_ERROR_CHECK(i2s_new_channel(&cc, &s_tx, NULL));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
}

static void play(const char *text) {
  bys_tx_cfg_t cfg;
  bys_tx_cfg_default(&cfg);
  cfg.sample_rate = RATE;
  cfg.level = CONFIG_BYSOUND_DEMO_TX_LEVEL / 100.0f;
  const int r = bys_tx_init_payload(&s_state, &cfg, BYS_TOP, BYS_NORMAL, text, strlen(text));
  if (r != BYS_OK) { ESP_LOGE(TAG, "cannot render the token: %d", r); return; }
  while (!bys_tx_done(&s_state)) {
    const size_t n = bys_tx_fill(&s_state, s_pcm, BLOCK);
    for (size_t i = 0; i < n; i++) s_buf32[2 * i] = s_buf32[2 * i + 1] = (int32_t)s_pcm[i] << 16;
    size_t written = 0;
    i2s_channel_write(s_tx, s_buf32, n * 2 * sizeof(int32_t), &written, portMAX_DELAY);
  }
}

void app_main(void) {
  time_init();
  speaker_init();
  ESP_LOGI(TAG, "store \"%s\": broadcasting on top every %d s", CONFIG_PRESENCE_STORE_ID, CONFIG_PRESENCE_INTERVAL_S);
  for (;;) {
    const int64_t t0 = esp_timer_get_time();
    char token[64];
    make_token(token, sizeof token, unix_now());
    ESP_LOGI(TAG, "playing %s", token);
    play(token);
    const int64_t spent_ms = (esp_timer_get_time() - t0) / 1000;
    const int64_t wait_ms = (int64_t)CONFIG_PRESENCE_INTERVAL_S * 1000 - spent_ms;
    vTaskDelay(pdMS_TO_TICKS(wait_ms > 500 ? wait_ms : 500));
  }
}
