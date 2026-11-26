#include "audio.h"

#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bysound.h"
#include "sdkconfig.h"

#define RATE        48000
#define BLOCK       480 /* frames per I2S transfer: 10 ms */
#define BAND        ((bys_band_t)CONFIG_BYSOUND_DEMO_BAND)
#ifdef CONFIG_BYSOUND_DEMO_PROFILE_ROBUST
#define PROFILE     BYS_ROBUST
#else
#define PROFILE     BYS_NORMAL
#endif

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx, s_rx;
static audio_text_cb_t s_on_text;

/* Receive side. The demodulator state is static: no malloc anywhere in the modem. */
static bys_demod_t s_demod;
static uint8_t s_packed[CONFIG_BYSOUND_DEMO_RX_MAX_BYTES];
static char s_text[2 * CONFIG_BYSOUND_DEMO_RX_MAX_BYTES + 1];

/* Transmit request, written by audio_send() and consumed by the audio task. */
static char s_tx_text[256];
static volatile int s_tx_repeat; /* > 0 means a request is pending; the task sets it back to 0 when done */

static int32_t s_rx32[BLOCK * 2];
static int16_t s_pcm[BLOCK];
static int32_t s_tx32[BLOCK * 2];

static void on_event(void *user, const bys_demod_event_t *ev) {
  (void)user;
  if (ev->type != BYS_EV_FRAME || ev->result->kind != BYS_KIND_PAYLOAD || !ev->payload) return;
  int n = bys_unpack(ev->payload, ev->payload_len, s_text, sizeof s_text);
  if (n < 0) { ESP_LOGW(TAG, "unpack failed (%d)", n); return; }
  if (s_on_text) s_on_text(s_text);
}

static esp_err_t i2s_setup(void) {
  /* Both ports run stereo 32-bit Philips at 48 kHz. The INMP441 (L/R pin to GND) talks in the left slot; the MAX98357A
     mixes left+right, so the same sample goes to both slots. */
  i2s_std_config_t cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = CONFIG_BYSOUND_DEMO_SPK_BCLK, .ws = CONFIG_BYSOUND_DEMO_SPK_WS,
                  .dout = CONFIG_BYSOUND_DEMO_SPK_DIN, .din = I2S_GPIO_UNUSED },
  };
  i2s_chan_config_t tx_cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  tx_cc.dma_frame_num = BLOCK;
  tx_cc.dma_desc_num = 6;
  tx_cc.auto_clear = true; /* silence when nothing is written */
  ESP_ERROR_CHECK(i2s_new_channel(&tx_cc, &s_tx, NULL));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &cfg));

  i2s_chan_config_t rx_cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  rx_cc.dma_frame_num = BLOCK;
  rx_cc.dma_desc_num = 6;
  ESP_ERROR_CHECK(i2s_new_channel(&rx_cc, NULL, &s_rx));
  cfg.gpio_cfg.bclk = CONFIG_BYSOUND_DEMO_MIC_BCLK;
  cfg.gpio_cfg.ws = CONFIG_BYSOUND_DEMO_MIC_WS;
  cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  cfg.gpio_cfg.din = CONFIG_BYSOUND_DEMO_MIC_SD;
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &cfg));

  ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
  return i2s_channel_enable(s_rx);
}

static void demod_start(void) {
  bys_demod_cfg_t cfg;
  bys_demod_cfg_default(&cfg);
  cfg.sample_rate = RATE;
  cfg.profile = PROFILE;
  cfg.band_mask = (uint8_t)(1u << BAND); /* listen only on the band in use: cheaper than the default four */
  cfg.out = s_packed;
  cfg.out_cap = sizeof s_packed;
  cfg.cb = on_event;
  int r = bys_demod_init(&s_demod, &cfg);
  if (r != BYS_OK) { ESP_LOGE(TAG, "bys_demod_init failed: %d", r); abort(); }
}

/* Plays the frame to the amplifier, blocking until the last sample (including the 350 ms gap) is queued. */
static void transmit(const char *text, int repeat) {
  bys_tx_cfg_t cfg;
  bys_tx_cfg_default(&cfg);
  cfg.sample_rate = RATE;
  cfg.level = CONFIG_BYSOUND_DEMO_TX_LEVEL / 100.0f;
  for (int k = 0; k < repeat; k++) {
    bys_tx_t tx;
    int r = bys_tx_init_payload(&tx, &cfg, BAND, PROFILE, text, strlen(text));
    if (r != BYS_OK) { ESP_LOGE(TAG, "cannot encode: %d", r); return; }
    ESP_LOGI(TAG, "sending %d/%d (%.1f s)", k + 1, repeat, (double)bys_tx_total_samples(&tx) / RATE);
    while (!bys_tx_done(&tx)) {
      size_t n = bys_tx_fill(&tx, s_pcm, BLOCK);
      for (size_t i = 0; i < n; i++) s_tx32[2 * i] = s_tx32[2 * i + 1] = (int32_t)s_pcm[i] << 16;
      size_t written = 0;
      i2s_channel_write(s_tx, s_tx32, n * 2 * sizeof(int32_t), &written, portMAX_DELAY);
    }
  }
}

static void audio_task(void *arg) {
  (void)arg;
  for (;;) {
    if (s_tx_repeat > 0) {
      transmit(s_tx_text, s_tx_repeat);
      s_tx_repeat = 0;
      bys_demod_reset(&s_demod); /* forget our own sound */
    }
    size_t got = 0;
    if (i2s_channel_read(s_rx, s_rx32, sizeof s_rx32, &got, 200) != ESP_OK) continue;
    const size_t n = got / (2 * sizeof(int32_t));
    for (size_t i = 0; i < n; i++) {
      /* 24-bit sample left-justified in 32 bits: keep the top 16, apply the digital gain, saturate. */
      int32_t v = (s_rx32[2 * i] >> 16) * (1 << CONFIG_BYSOUND_DEMO_MIC_GAIN_SHIFT);
      s_pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    bys_demod_push(&s_demod, s_pcm, n);
  }
}

esp_err_t audio_start(audio_text_cb_t on_text) {
  s_on_text = on_text;
  esp_err_t e = i2s_setup();
  if (e != ESP_OK) return e;
  demod_start();
  return xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 5, NULL, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t audio_send(const char *text, int repeat) {
  if (s_tx_repeat > 0) return ESP_ERR_INVALID_STATE;
  if (strlcpy(s_tx_text, text, sizeof s_tx_text) >= sizeof s_tx_text) ESP_LOGW(TAG, "text truncated to %u bytes", (unsigned)(sizeof s_tx_text - 1));
  s_tx_repeat = repeat;
  return ESP_OK;
}
