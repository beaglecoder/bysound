/* esp32-wifi-setup: receives Wi-Fi credentials by sound (the device half of examples/web-wifi-setup) and joins the network.
 *
 *  1. The board listens as a session receiver (audio code of ../esp32). It acknowledges the text by sound by itself (the session ack).
 *  2. A one-time code is printed on the serial log (valid CONFIG_WIFISETUP_CODE_SECONDS); the person types it on the web page.
 *  3. Once the text passed the confirm gate it is opened (protected mode) or parsed (plain mode, only if enabled), the board joins the
 *     network as a station and, only if it got an IP address, stores the credentials in NVS. */
#include <string.h>

#include "audio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wifi_text.h"

static const char *TAG = "wifi_setup";
#define BIT_GOT_IP BIT0
#define BIT_FAIL   BIT1
#define NS "wifi_setup"

static EventGroupHandle_t s_ev;
static esp_netif_t *s_netif;
static int s_retries, s_max_retries;
static TaskHandle_t s_worker;

/* Message handed from the audio task to the worker (PBKDF2 takes seconds: the audio task must keep listening and acking). */
static char s_msg[640];
static volatile bool s_msg_full;

static char s_code[WIFI_CODE_LENGTH + 1];
static int64_t s_code_until_us; /* 0: no code active */
static int s_attempts;

static void issue_code(const char *why) {
  wifi_text_new_code(s_code);
  s_code_until_us = esp_timer_get_time() + (int64_t)CONFIG_WIFISETUP_CODE_SECONDS * 1000000;
  s_attempts = 0;
  ESP_LOGW(TAG, "================================================");
  ESP_LOGW(TAG, "ONE-TIME CODE (%s): %s   valid %d s, %d attempts", why, s_code, CONFIG_WIFISETUP_CODE_SECONDS, CONFIG_WIFISETUP_MAX_ATTEMPTS);
  ESP_LOGW(TAG, "================================================");
}
static void burn_code(void) { mbedtls_platform_zeroize(s_code, sizeof s_code); s_code_until_us = 0; }

/* ---------- Wi-Fi ---------- */
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg; (void)data;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) { ESP_LOGI(TAG, "wifi: station started, connecting"); esp_wifi_connect(); }
  else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const wifi_event_sta_disconnected_t *d = data;
    ESP_LOGW(TAG, "wifi: disconnected (reason %d), attempt %d", d->reason, s_retries + 1);
    if (s_max_retries < 0 || s_retries++ < s_max_retries) esp_wifi_connect();
    else xEventGroupSetBits(s_ev, BIT_FAIL);
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *g = data;
    ESP_LOGI(TAG, "wifi: got IP " IPSTR, IP2STR(&g->ip_info.ip));
    xEventGroupSetBits(s_ev, BIT_GOT_IP);
  }
}

static void wifi_init(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  s_netif = esp_netif_create_default_wifi_sta();
  const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); /* our own NVS record is the one that counts */
}

/* Returns true if the board got an IP with these credentials. max_retries < 0: never gives up. */
static bool wifi_join(const wifi_creds_t *c, int max_retries, int timeout_s) {
  wifi_config_t wc = { 0 };
  strlcpy((char *)wc.sta.ssid, c->ssid, sizeof wc.sta.ssid);
  strlcpy((char *)wc.sta.password, c->password, sizeof wc.sta.password);
  wc.sta.threshold.authmode = c->open ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK; /* WPA, WPA2 and WPA3 transition networks */
  wc.sta.pmf_cfg.capable = true;
  s_retries = 0; s_max_retries = max_retries;
  xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_FAIL);
  esp_wifi_stop(); /* a no-op if it was not started */
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  mbedtls_platform_zeroize(&wc, sizeof wc);
  ESP_LOGI(TAG, "wifi: joining \"%s\" (%s)", c->ssid, c->open ? "open" : "WPA");
  ESP_ERROR_CHECK(esp_wifi_start());
  EventBits_t b = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_FAIL, pdFALSE, pdFALSE, timeout_s > 0 ? pdMS_TO_TICKS(timeout_s * 1000) : portMAX_DELAY);
  return (b & BIT_GOT_IP) != 0;
}

/* ---------- NVS ---------- */
static bool creds_save(const wifi_creds_t *c) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t e = nvs_set_blob(h, "creds", c, sizeof *c);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK;
}
static bool creds_load(wifi_creds_t *c) {
  nvs_handle_t h;
  size_t n = sizeof *c;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
  esp_err_t e = nvs_get_blob(h, "creds", c, &n);
  nvs_close(h);
  return e == ESP_OK && n == sizeof *c;
}

/* ---------- message handling ---------- */
/* Called by the audio code, in its task, once the text was delivered AND the confirm gate is clean. Only hands the text over. */
static void on_text(const char *text) {
  const size_t n = strlen(text);
  ESP_LOGI(TAG, "text received and confirmed (%u bytes, %s)", (unsigned)n, strncmp(text, "BSW1|", 5) == 0 ? "protected" : strncmp(text, "WIFI:", 5) == 0 ? "plain" : "unknown");
  if (s_msg_full) { ESP_LOGW(TAG, "still busy with the previous text; ignored"); return; }
  if (n >= sizeof s_msg) { ESP_LOGW(TAG, "text too long for a Wi-Fi setup; ignored"); return; }
  memcpy(s_msg, text, n + 1);
  s_msg_full = true;
  xTaskNotifyGive(s_worker);
}

static void handle(char *text) {
  char plain[256];
  const char *wifi = NULL;
  if (strncmp(text, "BSW1|", 5) == 0) {
    if (!s_code_until_us || esp_timer_get_time() > s_code_until_us) { ESP_LOGW(TAG, "protected text but no valid code (expired); ignored"); return; }
    ESP_LOGI(TAG, "opening with the one-time code (PBKDF2 200000 iterations, takes a few seconds)...");
    const int64_t t0 = esp_timer_get_time();
    const int n = wifi_text_unprotect(text, s_code, plain, sizeof plain);
    ESP_LOGI(TAG, "key derivation and decryption took %.1f s", (esp_timer_get_time() - t0) / 1e6);
    if (n == -1) { ESP_LOGW(TAG, "malformed protected text; ignored"); return; }
    if (n < 0) {
      ESP_LOGW(TAG, "wrong code or tampered text (attempt %d of %d)", ++s_attempts, CONFIG_WIFISETUP_MAX_ATTEMPTS);
      if (s_attempts >= CONFIG_WIFISETUP_MAX_ATTEMPTS) { burn_code(); issue_code("too many attempts"); }
      return;
    }
    burn_code(); /* authenticated: the code is spent */
    ESP_LOGI(TAG, "decrypted ok, code burnt");
    wifi = plain;
  } else if (strncmp(text, "WIFI:", 5) == 0) {
#ifdef CONFIG_WIFISETUP_ALLOW_PLAIN
    ESP_LOGW(TAG, "plain text accepted (CONFIG_WIFISETUP_ALLOW_PLAIN): credentials were in clear in the air");
    wifi = text;
#else
    ESP_LOGW(TAG, "plain WIFI: text refused (protected mode only; enable CONFIG_WIFISETUP_ALLOW_PLAIN to accept it)");
    return;
#endif
  } else { ESP_LOGW(TAG, "not a Wi-Fi setup text; ignored"); return; }

  wifi_creds_t c;
  const int r = wifi_text_parse(wifi, &c);
  mbedtls_platform_zeroize(plain, sizeof plain);
  if (r != 0) { ESP_LOGW(TAG, "cannot use the credentials (parse error %d: %s)", r, r == -3 ? "ssid/password length" : r == -2 ? "WEP or no ssid" : "not WIFI:"); if (!s_code_until_us) issue_code("retry"); return; }
  ESP_LOGI(TAG, "credentials: ssid \"%s\", %s, password %u chars", c.ssid, c.open ? "open" : "WPA", (unsigned)strlen(c.password));

  if (wifi_join(&c, 5, CONFIG_WIFISETUP_CONNECT_TIMEOUT_S)) {
    if (creds_save(&c)) ESP_LOGI(TAG, "credentials stored in NVS (namespace %s); setup done", NS);
    else ESP_LOGE(TAG, "connected but NVS write failed");
  } else {
    ESP_LOGW(TAG, "could not join \"%s\" (wrong password, out of range or no DHCP); nothing stored", c.ssid);
    esp_wifi_stop();
    issue_code("retry after failed join");
  }
  mbedtls_platform_zeroize(&c, sizeof c);
}

static void worker(void *arg) {
  (void)arg;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    if (s_msg_full) { handle(s_msg); mbedtls_platform_zeroize(s_msg, sizeof s_msg); s_msg_full = false; }
    else if (s_code_until_us && esp_timer_get_time() > s_code_until_us) { ESP_LOGW(TAG, "code expired"); burn_code(); issue_code("renewed"); }
  }
}

void app_main(void) {
  esp_err_t e = nvs_flash_init(); /* audio_start() does it too; harmless twice */
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); ESP_ERROR_CHECK(nvs_flash_init()); }
  s_ev = xEventGroupCreate();
  wifi_init();
  ESP_ERROR_CHECK(xTaskCreatePinnedToCore(worker, "wifi_setup", 8192, NULL, 4, &s_worker, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

  ESP_ERROR_CHECK(audio_start(on_text));
  issue_code("first");
  ESP_LOGI(TAG, "listening for a Wi-Fi setup text (send it from examples/web-wifi-setup using the code above)");

  wifi_creds_t c;
  if (creds_load(&c)) {
    ESP_LOGI(TAG, "stored credentials for \"%s\": joining (a new text replaces them)", c.ssid);
    if (wifi_join(&c, -1, 15)) ESP_LOGI(TAG, "connected with the stored credentials");
    else ESP_LOGW(TAG, "not connected yet; still retrying in the background");
    mbedtls_platform_zeroize(&c, sizeof c);
  } else ESP_LOGI(TAG, "no stored credentials");

}
