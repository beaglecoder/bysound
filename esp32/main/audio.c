#include "audio.h"

#include <string.h>

#include "bysound.h"
#include "bysound_session.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define RATE        48000
#define BLOCK       480 /* frames per I2S transfer: 10 ms */
#define CONFIRM_MS  2000 /* the demo acts on a received text this long after delivery, if the confirm gate is still clean */

#if defined(CONFIG_BYSOUND_DEMO_SECURITY_BASIC)
#define SECURITY    BYS_SEC_BASIC
#elif defined(CONFIG_BYSOUND_DEMO_SECURITY_STANDARD)
#define SECURITY    BYS_SEC_STANDARD
#else
#define SECURITY    BYS_SEC_MAXIMUM
#endif

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx, s_rx;
static audio_text_cb_t s_on_text;

/* Receive side. All state is static: no malloc anywhere in the modem or the session. One demodulator per profile, the same audio into both. */
static bys_demod_t s_dn;
static uint8_t s_packed_n[CONFIG_BYSOUND_DEMO_RX_MAX_BYTES];
#ifdef CONFIG_BYSOUND_DEMO_ROBUST_RX
static bys_demod_t s_dr;
static uint8_t s_packed_r[CONFIG_BYSOUND_DEMO_RX_MAX_BYTES];
#endif
static char s_text[2 * CONFIG_BYSOUND_DEMO_RX_MAX_BYTES + 1], s_scratch[256];

/* Session state: the board is a receiver until the button asks for a send; then a sender until it is done. */
static bys_sender_t s_snd;
static bys_receiver_t s_rcv;
static bys_risk_memory_t s_mem;
static bys_session_io s_io_snd, s_io_rcv;
static char s_tx_text[256];
static volatile int s_send_req;          /* set by audio_send(), consumed by the task */
static int s_snd_on, s_rcv_on;           /* 1 running, 0 off, -1 finished (the task cleans up) */
static int s_rcv_confirmed;
static uint32_t s_rcv_delivered_at, s_noise_at;

/* Playback of one session frame at a time (half duplex: the microphone is not read while it plays). */
static bys_tx_t s_tx_state;
static int s_playing;

static int32_t s_rx32[BLOCK * 2];
static int16_t s_pcm[BLOCK];
static int32_t s_tx32[BLOCK * 2];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static const char *const ERR_NAMES[] = { "none", "OTHER_TRANSMISSION", "CLONED_TRANSMISSION", "OTHER_EMITTER", "RISK_SIGNAL", "MULTIPLE_TRANSMISSIONS", "TIMEOUT",
                                         "STOPPED", "CANCELLED", "NOT_FOUND", "MISMATCH", "NO_RESOLVER", "PLAY", "NO_SPACE", "PENDING" };
static const char *const BAND_NAMES[] = { "top", "high", "upper", "mid", "audible", "xhigh", "wide" };

/* ---------- I2S ---------- */
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

/* ---------- session hooks ---------- */
static int io_play(void *u, const bys_session_frame_t *f) {
  (void)u;
  bys_tx_cfg_t cfg;
  bys_tx_cfg_default(&cfg);
  cfg.sample_rate = RATE;
  cfg.level = CONFIG_BYSOUND_DEMO_TX_LEVEL / 100.0f;
  int r = bys_session_tx_init(&s_tx_state, &cfg, f);
  if (r != BYS_OK) { ESP_LOGE(TAG, "cannot render frame %c: %d", f->kind, r); return r; }
  s_playing = 1;
  return BYS_OK;
}
static void io_stop(void *u) { (void)u; s_playing = 0; }
static uint32_t io_random(void *u) { (void)u; return esp_random(); }

/* Calibration memory (SESSION.md 9.4) in NVS: where the last receiver read this speaker. */
static int io_cal_load(void *u, bys_calibration_t *c) {
  (void)u;
  nvs_handle_t h;
  uint8_t b[3];
  size_t n = sizeof b;
  if (nvs_open("bysound", NVS_READONLY, &h) != ESP_OK) return 0;
  esp_err_t e = nvs_get_blob(h, "cal", b, &n);
  nvs_close(h);
  if (e != ESP_OK || n != sizeof b) return 0;
  c->band = b[0]; c->profile = b[1]; c->uses = b[2];
  return 1;
}
static void io_cal_save(void *u, const bys_calibration_t *c) {
  (void)u;
  nvs_handle_t h;
  if (nvs_open("bysound", NVS_READWRITE, &h) != ESP_OK) return;
  if (c) { const uint8_t b[3] = { c->band, c->profile, c->uses }; nvs_set_blob(h, "cal", b, sizeof b); }
  else nvs_erase_key(h, "cal");
  nvs_commit(h);
  nvs_close(h);
}

static void io_alert(void *u, const bys_alert_t *a) {
  (void)u;
  ESP_LOGW(TAG, "risk frame %s: reason %u, source %s, hops %u, relay %d", a->heard ? "heard" : "emitted", a->reason, a->source ? "receiver" : "sender", a->hops, a->relay);
}
static void io_quarantine(void *u, uint32_t ms, uint8_t reason, uint8_t source) {
  (void)u; (void)source;
  ESP_LOGW(TAG, "suggested pause %lu s after a risk signal (reason %u); nothing is blocked", (unsigned long)(ms / 1000), reason);
}
static void io_repetition(void *u, uint32_t i, bys_band_t band, bys_profile_t profile) {
  (void)u;
  ESP_LOGI(TAG, "sending #%lu on %s/%s", (unsigned long)i, BAND_NAMES[band], profile == BYS_ROBUST ? "robust" : "normal");
}
static void io_delivered(void *u, const bys_delivered_t *d) {
  (void)u;
  ESP_LOGI(TAG, "acked: read on %s/%s%s after %lu repetition(s), %.1f s%s", BAND_NAMES[d->band], d->profile == BYS_ROBUST ? "robust" : "normal", d->stated ? "" : " (not stated)",
           (unsigned long)d->repetitions, d->ms / 1000.0, d->saved ? "; calibration saved" : "");
}
static void io_received(void *u, const char *text, size_t len, bys_band_t band, int via_reference) {
  (void)u; (void)text;
  ESP_LOGI(TAG, "delivered %u bytes heard on %s%s; acknowledging", (unsigned)len, BAND_NAMES[band], via_reference ? " (reference)" : "");
  s_rcv_delivered_at = now_ms();
  s_rcv_confirmed = 0;
}
static void io_receipt(void *u, int heard) { (void)u; if (heard) ESP_LOGI(TAG, "receipt heard: the sender knows"); else ESP_LOGI(TAG, "no receipt (acking ended)"); }
static void io_failed(void *u, bys_session_error_t e, const bys_alert_t *a) { (void)u; (void)a; ESP_LOGW(TAG, "session failed: %s", ERR_NAMES[e]); }
static void snd_finished(void *u) { (void)u; s_snd_on = -1; }
static void rcv_finished(void *u) { (void)u; s_rcv_on = -1; }

static void io_init(void) {
  s_io_snd = (bys_session_io){ .play = io_play, .stop = io_stop, .random = io_random, .cal_load = io_cal_load, .cal_save = io_cal_save, .on_alert = io_alert,
                               .on_quarantine = io_quarantine, .on_repetition = io_repetition, .on_delivered = io_delivered, .on_received = io_received,
                               .on_receipt = io_receipt, .on_failed = io_failed, .on_finished = snd_finished };
  s_io_rcv = s_io_snd;
  s_io_rcv.on_finished = rcv_finished;
  bys_risk_memory_init(&s_mem);
}

static void on_event(void *user, const bys_demod_event_t *ev) {
  (void)user;
  if (s_snd_on > 0) bys_sender_hear(&s_snd, now_ms(), ev);
  else if (s_rcv_on > 0) bys_receiver_hear(&s_rcv, now_ms(), ev);
}

static void demod_start(void) {
  bys_demod_cfg_t cfg;
  bys_demod_cfg_default(&cfg); /* top, high, upper and mid: the whole ladder, and acks on mid */
  cfg.sample_rate = RATE;
  cfg.cb = on_event;
  cfg.out = s_packed_n;
  cfg.out_cap = sizeof s_packed_n;
  int r = bys_demod_init(&s_dn, &cfg);
  if (r != BYS_OK) { ESP_LOGE(TAG, "bys_demod_init failed: %d", r); abort(); }
#ifdef CONFIG_BYSOUND_DEMO_ROBUST_RX
  cfg.profile = BYS_ROBUST; /* the sender goes robust after a few repetitions: hear both profiles */
  cfg.out = s_packed_r;
  cfg.out_cap = sizeof s_packed_r;
  r = bys_demod_init(&s_dr, &cfg);
  if (r != BYS_OK) { ESP_LOGE(TAG, "bys_demod_init (robust) failed: %d", r); abort(); }
#endif
}

static void demod_reset(void) {
  bys_demod_reset(&s_dn); /* forget our own sound */
#ifdef CONFIG_BYSOUND_DEMO_ROBUST_RX
  bys_demod_reset(&s_dr);
#endif
}

static void receiver_start(void) {
  bys_receiver_cfg_t c;
  bys_receiver_cfg_default(&c, s_text, sizeof s_text);
  bys_security_resolve(&c.security, SECURITY);
  c.scratch = s_scratch; c.scratch_cap = sizeof s_scratch; c.memory = &s_mem;
  s_rcv_on = bys_receiver_start(&s_rcv, &c, &s_io_rcv, now_ms()) == BYS_OK;
}

static void sender_start(void) {
  bys_sender_cfg_t c;
  bys_sender_cfg_default(&c, s_tx_text, strlen(s_tx_text));
  bys_security_resolve(&c.security, SECURITY);
  c.scratch = s_scratch; c.scratch_cap = sizeof s_scratch; c.memory = &s_mem;
#ifdef CONFIG_BYSOUND_DEMO_FIXED_BAND
  c.band = CONFIG_BYSOUND_DEMO_BAND; /* no ladder, no calibration */
#endif
  s_snd_on = bys_sender_start(&s_snd, &c, &s_io_snd, now_ms()) == BYS_OK;
  s_noise_at = now_ms();
  if (s_snd_on) ESP_LOGI(TAG, "send: pre-listening, then \"%s\"", s_tx_text);
}

/* Timers of the running session. The sender's pre-listen measures the noise of each channel (every 0.25 s) to skip the loud ones. */
static void session_tick(uint32_t now) {
  if (s_snd_on > 0) {
    if (bys_sender_state(&s_snd) == BYS_SS_PRE_LISTEN && now - s_noise_at >= BYS_NOISE_REPORT_MS) {
      float db[BYS_BAND_COUNT];
      bys_session_noise_from_demod(&s_dn, db);
      bys_sender_noise(&s_snd, now, db);
      s_noise_at = now;
    }
    bys_sender_tick(&s_snd, now);
  } else if (s_rcv_on > 0) {
    bys_receiver_tick(&s_rcv, now);
    /* The confirm gate: act on the text only if nothing else was heard meanwhile. */
    if (!s_rcv_confirmed && bys_receiver_state(&s_rcv) == BYS_RS_DELIVERED && now - s_rcv_delivered_at >= CONFIRM_MS) {
      bys_confirmation_t cf;
      const int e = bys_receiver_confirmed(&s_rcv, now, &cf);
      s_rcv_confirmed = 1;
      if (e != 0) ESP_LOGW(TAG, "not confirmed: %s", ERR_NAMES[e]);
      else {
        ESP_LOGI(TAG, "confirmed (watch %s, %lu ms)", cf.watch == BYS_WATCH_CLEAN ? "clean" : cf.watch == BYS_WATCH_OFF ? "off" : "expired", (unsigned long)cf.watched_ms);
        if (s_on_text) s_on_text(s_text);
      }
    }
  }
}

static void audio_task(void *arg) {
  (void)arg;
  receiver_start();
  for (;;) {
    if (s_snd_on < 0) { s_snd_on = 0; receiver_start(); }          /* send over: listen again */
    else if (s_rcv_on < 0) { s_rcv_on = 0; receiver_start(); }     /* text delivered and acknowledged (or failed): the next one */
    if (s_send_req && s_snd_on <= 0) {                             /* the button: stop receiving, send */
      s_send_req = 0;
      if (s_rcv_on > 0) { bys_receiver_cancel(&s_rcv, now_ms()); }
      s_rcv_on = 0;
      sender_start();
    }
    if (s_playing) {
      const size_t n = bys_tx_fill(&s_tx_state, s_pcm, BLOCK);
      for (size_t i = 0; i < n; i++) s_tx32[2 * i] = s_tx32[2 * i + 1] = (int32_t)s_pcm[i] << 16;
      size_t written = 0;
      i2s_channel_write(s_tx, s_tx32, n * 2 * sizeof(int32_t), &written, portMAX_DELAY);
      if (s_playing && bys_tx_done(&s_tx_state)) { /* its last sample (gap included) is queued: tell the session */
        s_playing = 0;
        size_t got = 0;
        while (i2s_channel_read(s_rx, s_rx32, sizeof s_rx32, &got, 0) == ESP_OK && got) {} /* drop what the microphone heard of us */
        demod_reset();
        if (s_snd_on > 0) bys_sender_play_done(&s_snd, now_ms());
        else if (s_rcv_on > 0) bys_receiver_play_done(&s_rcv, now_ms());
      }
    } else {
      size_t got = 0;
      if (i2s_channel_read(s_rx, s_rx32, sizeof s_rx32, &got, 200) == ESP_OK) {
        const size_t n = got / (2 * sizeof(int32_t));
        for (size_t i = 0; i < n; i++) {
          /* 24-bit sample left-justified in 32 bits: keep the top 16, apply the digital gain, saturate. */
          int32_t v = (s_rx32[2 * i] >> 16) * (1 << CONFIG_BYSOUND_DEMO_MIC_GAIN_SHIFT);
          s_pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        bys_demod_push(&s_dn, s_pcm, n);
#ifdef CONFIG_BYSOUND_DEMO_ROBUST_RX
        bys_demod_push(&s_dr, s_pcm, n);
#endif
      }
    }
    session_tick(now_ms());
  }
}

esp_err_t audio_start(audio_text_cb_t on_text) {
  s_on_text = on_text;
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); e = nvs_flash_init(); }
  if (e != ESP_OK) return e;
  e = i2s_setup();
  if (e != ESP_OK) return e;
  io_init();
  demod_start();
  return xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 5, NULL, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t audio_send(const char *text) {
  if (s_send_req || s_snd_on > 0) return ESP_ERR_INVALID_STATE;
  if (strlcpy(s_tx_text, text, sizeof s_tx_text) >= sizeof s_tx_text) ESP_LOGW(TAG, "text truncated to %u bytes", (unsigned)(sizeof s_tx_text - 1));
  s_send_req = 1;
  return ESP_OK;
}
