# esp32-wifi-setup (TODO)

**Status: planned.** Waits for the C session layer (`c/`, docs/SESSION.md). It is the device half of `../web-wifi-setup`.

Planned firmware (ESP-IDF, using the `bysound` component): on first boot, or after a button hold, the device (1) shows an 8-character one-time code on its display (or prints it on serial) valid for 120 s, (2) listens with the demodulator on the microphone, (3) on a `BSW1|salt|iv|ciphertext` message derives the key with mbedtls PBKDF2-SHA256 (200000 iterations) from the code, opens it with AES-GCM, parses `WIFI:T:WPA;S:..;P:..;;`, burns the code and calls `esp_wifi_set_config` / connects, (4) acknowledges by sound with the session layer's ack so the phone shows "delivered", and stores the credentials in NVS. Plain `WIFI:` messages are refused unless a build option allows them. Documents the same limits as the web page: sound is not secret, 40 bits is offline-guessable.
