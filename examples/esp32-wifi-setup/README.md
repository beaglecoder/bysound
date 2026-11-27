# esp32-wifi-setup

The device half of [`../web-wifi-setup`](../web-wifi-setup/): an ESP32-S3 that receives Wi-Fi credentials by sound, joins the network and keeps the credentials.

**Status: it builds (ESP-IDF 5.4, ESP32-S3, app 0xc2f10 bytes = about 800 KB of a 1.5 MB partition). It has NOT been run on a board**: the audio path is the one of [`../../esp32`](../../esp32/) (also not yet run on a board), and the Wi-Fi/crypto code here has only had its text parser checked on a host. Expect to debug it on first run.

## What it shows

- Listening with the session receiver (`c/include/bysound_session.h`) wired to a microphone, exactly as in `esp32/`: the receiver acknowledges the text by sound by itself, which is the only "it arrived" signal the phone gets. The example reuses `esp32/main/audio.c` and the `bysound` component by relative path (no copy).
- Acting only after the confirm gate (`bys_receiver_confirmed` clean, 2 s after delivery): the audio code calls `on_text` only then.
- **Protected mode** (default), compatible with `web-wifi-setup/format.ts`: the text is `BSW1|<salt b64>|<iv b64>|<ciphertext+tag b64>`; key = PBKDF2-HMAC-SHA256(code normalized to `[A-Z0-9]`, salt, 200000 iterations, 32 bytes), AES-256-GCM, 12-byte IV, 16-byte tag appended to the ciphertext (WebCrypto layout). Done with mbedTLS in a separate task so the audio task keeps listening.
- The one-time code (8 symbols from the web's 31-symbol alphabet) is **printed on the serial log** (no display or LED code), valid 120 s, 3 wrong attempts, then renewed. An authenticated decryption burns it.
- The `WIFI:T:WPA;S:..;P:..;;` parser with the web's escaping (`\` before `; , : "`), `T:nopass` for open networks; WEP is refused.
- After the text: join as a station (`esp_wifi`), and **only when an IP was obtained** store `ssid`/`password` in NVS (namespace `wifi_setup`). A failed join stores nothing and issues a new code. On the next boot stored credentials are used right away.
- Every step is logged (`wifi_setup` tag).

## Hardware and wiring

Same as the esp32 demo (INMP441 mic + MAX98357A amplifier, same pins and Kconfig): see [`../../esp32/README.md`](../../esp32/README.md).

## Build and flash

```sh
. ~/esp/esp-idf/export.sh
cd examples/esp32-wifi-setup
idf.py set-target esp32s3
idf.py menuconfig        # "bysound demo" (pins, gain, security level) and "Wi-Fi setup by sound"
idf.py build
idf.py -p PORT flash monitor
```

Use it: the monitor prints `ONE-TIME CODE (first): XXXXXXXX`. On `web-wifi-setup` choose the protected mode, enter that code, the network name and password, and send. Expected log: `text received and confirmed (... protected)`, `opening with the one-time code`, `decrypted ok, code burnt`, `credentials: ssid ...`, `wifi: got IP ...`, `credentials stored in NVS`.

Options (menuconfig): accept plain texts (`WIFISETUP_ALLOW_PLAIN`, off), code validity, attempts, connect timeout. Wi-Fi and the two demodulators use about 250 KB of internal RAM in total (DIRAM 74 % of the S3 build, no PSRAM needed).

## Security note

**Sound is not secret.** Anyone in earshot (or a recording) gets everything that is sent. Plain `WIFI:` texts pass the credentials in clear, so they are refused unless you enable `WIFISETUP_ALLOW_PLAIN`. The protected mode hides them behind the one-time code, but the code has only about 40 bits: a recording can be attacked offline (200000 PBKDF2 iterations slow it, they do not stop a determined attacker). It protects against someone who hears the exchange but cannot afford that attack, not against one who records and has the compute; the physical presence of the person who reads the code from the device is the real factor. The code is shown only on the serial log here; a product should show it on a display. Do not reuse your main Wi-Fi password if that matters.

## Limits

- Not run on a board; PBKDF2 duration on the S3 (hardware SHA enabled) is not measured.
- The code is only logged: no on-device display or LED.
- No button-triggered re-provisioning: a new code is issued on boot and when one expires; to change the network after storing one, send a new text with the current code (the stored credentials are replaced on a successful join) or erase NVS.
- WEP and enterprise networks are not supported.
- The firmware keeps listening and acking by the demo's generic receiver; it does not send anything beyond the session ack.
