/* Pure text and crypto helpers of the Wi-Fi setup example (same formats as ../web-wifi-setup/format.ts). */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct { char ssid[33]; char password[64]; bool open; } wifi_creds_t;

#define WIFI_CODE_LENGTH 8
/* Fills code[WIFI_CODE_LENGTH + 1] with random symbols of the web's alphabet (no look-alikes). */
void wifi_text_new_code(char code[WIFI_CODE_LENGTH + 1]);

/* Parses "WIFI:T:WPA;S:<ssid>;P:<password>;;" (special chars \ ; , : " escaped with \). Returns 0 or a negative error:
 * -1 not a WIFI: text, -2 unsupported type (WEP) or missing ssid, -3 ssid/password length invalid for the device. */
int wifi_text_parse(const char *text, wifi_creds_t *out);

/* Opens "BSW1|<salt b64>|<iv b64>|<AES-GCM ciphertext+tag b64>" with PBKDF2-SHA256(code, salt, 200000) -> AES-256-GCM.
 * Returns the plain length (NUL terminated in out), or a negative error: -1 format, -2 wrong code or tampered text. */
int wifi_text_unprotect(const char *text, const char *code, char *out, size_t cap);
