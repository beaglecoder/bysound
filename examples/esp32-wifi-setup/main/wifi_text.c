#include "wifi_text.h"

#include <string.h>

#include "esp_random.h"
#include "mbedtls/base64.h"
#include "mbedtls/gcm.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"

#define ITERATIONS 200000
static const char ALPHABET[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"; /* 31 symbols, as CODE_ALPHABET in format.ts */

void wifi_text_new_code(char code[WIFI_CODE_LENGTH + 1]) {
  for (int i = 0; i < WIFI_CODE_LENGTH; i++) code[i] = ALPHABET[(esp_random() & 0xff) % (sizeof ALPHABET - 1)]; /* same mapping as the web: byte % 31 */
  code[WIFI_CODE_LENGTH] = 0;
}

/* Value of the field `key` after "WIFI:": starts at the beginning or after an unescaped ';', runs up to the next unescaped ';'. Unescapes \X -> X. */
static int field(const char *s, char key, char *out, size_t cap) {
  const char *p = s;
  while (*p) {
    if (p[0] == key && p[1] == ':') {
      size_t n = 0;
      p += 2;
      while (*p && *p != ';') {
        if (*p == '\\' && p[1]) p++;
        if (n + 1 >= cap) return -1;
        out[n++] = *p++;
      }
      out[n] = 0;
      return 0;
    }
    while (*p && *p != ';') p += (*p == '\\' && p[1]) ? 2 : 1; /* skip this field */
    if (*p == ';') p++;
  }
  return -2; /* absent */
}

int wifi_text_parse(const char *text, wifi_creds_t *out) {
  if (strncmp(text, "WIFI:", 5) != 0) return -1;
  const char *body = text + 5;
  char type[16];
  if (field(body, 'T', type, sizeof type) != 0) strcpy(type, "WPA"); /* absent: WPA, as the web */
  if (strcmp(type, "WPA") != 0 && strcmp(type, "nopass") != 0) return -2; /* WEP is not supported */
  memset(out, 0, sizeof *out);
  if (field(body, 'S', out->ssid, sizeof out->ssid) != 0) return -2;
  if (field(body, 'P', out->password, sizeof out->password) != 0) out->password[0] = 0;
  out->open = strcmp(type, "nopass") == 0;
  const size_t sl = strlen(out->ssid), pl = strlen(out->password);
  if (sl < 1 || sl > 32) return -3;
  if (!out->open && (pl < 8 || pl > 63)) return -3; /* WPA passphrase */
  return 0;
}

static size_t b64(const char *s, size_t n, uint8_t *out, size_t cap) {
  size_t w = 0;
  return mbedtls_base64_decode(out, cap, &w, (const unsigned char *)s, n) == 0 ? w : (size_t)-1;
}

int wifi_text_unprotect(const char *text, const char *code, char *out, size_t cap) {
  if (strncmp(text, "BSW1|", 5) != 0) return -1;
  const char *f[3] = { text + 5 }, *e[3];
  int k = 0;
  for (const char *p = text + 5; *p && k < 2; p++) if (*p == '|') { e[k++] = p; f[k] = p + 1; }
  if (k != 2) return -1;
  e[2] = text + strlen(text);
  uint8_t salt[16], iv[16], ct[320];
  const size_t ns = b64(f[0], e[0] - f[0], salt, sizeof salt), ni = b64(f[1], e[1] - f[1], iv, sizeof iv), nc = b64(f[2], e[2] - f[2], ct, sizeof ct);
  if (ns == (size_t)-1 || ns == 0 || ni != 12 || nc == (size_t)-1 || nc < 16 + 1 || nc - 16 >= cap) return -1;

  /* normalize(code): upper case, only A-Z0-9 (the code is typed by the person, but here it is the one we generated) */
  char norm[WIFI_CODE_LENGTH * 2 + 1];
  size_t nn = 0;
  for (const char *p = code; *p && nn < sizeof norm - 1; p++) {
    char c = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) norm[nn++] = c;
  }
  uint8_t key[32];
  int r = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)norm, nn, salt, ns, ITERATIONS, sizeof key, key);
  if (r != 0) return -2;

  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  r = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
  const size_t pl = nc - 16; /* WebCrypto appends the 16-byte tag to the ciphertext */
  if (r == 0) r = mbedtls_gcm_auth_decrypt(&g, pl, iv, ni, NULL, 0, ct + pl, 16, ct, (unsigned char *)out);
  mbedtls_gcm_free(&g);
  mbedtls_platform_zeroize(key, sizeof key);
  mbedtls_platform_zeroize(norm, sizeof norm);
  if (r != 0) return -2;
  out[pl] = 0;
  return (int)pl;
}
