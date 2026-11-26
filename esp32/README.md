# bysound_demo (ESP32-S3)

Listens continuously for bysound v4 messages, acknowledges each one by sound and logs it over serial. Press BOOT (GPIO0) to send a text (default
`hello from esp32`) and wait for the ack. It uses the session layer ([`docs/SESSION.md`](../docs/SESSION.md), `c/include/bysound_session.h`): as a receiver the board
delivers the text, plays acks on `mid` until the sender's receipt, and acts on the text (`main.c` logs it) once the confirm gate is still clean 2 s later; as a
sender it pre-listens, steps down the channel ladder (top, high, upper, mid, then robust) and stops as soon as it hears the ack, logging the band that worked
and keeping the calibration in NVS so the next send starts there. Uses the [`../c`](../c) library (wrapped as the `bysound` component in `components/bysound`) and the I2S standard driver
(`driver/i2s_std.h`). Tested with ESP-IDF v5.4.

## Hardware

Any ESP32-S3 board, an INMP441 I2S MEMS microphone and a MAX98357A I2S amplifier with a small speaker. Default pins (change them in
`idf.py menuconfig` under "bysound demo"):

| Module | Pin | ESP32-S3 GPIO |
|---|---|---|
| INMP441 | SCK | 4 |
| INMP441 | WS | 5 |
| INMP441 | SD | 6 |
| INMP441 | L/R | GND (left slot) |
| INMP441 | VDD / GND | 3V3 / GND |
| MAX98357A | BCLK | 15 |
| MAX98357A | LRC | 16 |
| MAX98357A | DIN | 17 |
| MAX98357A | VIN / GND | 5V / GND |
| Button | BOOT | 0 (to GND, internal pull-up) |

The microphone uses I2S port 1 and the amplifier I2S port 0, each with its own clocks, both at 48 kHz.

## Build and flash

```sh
. ~/esp/esp-idf/export.sh          # your ESP-IDF install
cd esp32
idf.py set-target esp32s3
idf.py menuconfig                  # optional: "bysound demo" (band, profile, text, pins, gain) and "bysound" (demodulator)
idf.py build
idf.py -p PORT flash monitor       # PORT is e.g. /dev/ttyACM0 or /dev/cu.usbmodem1101
```

Expected log: `listening; press the button ...`, then for every text heard `delivered N bytes heard on top; acknowledging`, `confirmed (watch clean, ...)` and
`received: <text>`; after a button press `send: pre-listening ...`, `sending #0 on top/normal`, ... and `acked: read on top/normal after 1 repetition(s) ...; calibration saved`.
Two boards running this demo talk to each other. To try one board alone, play a WAV made on the host (`c/build/bysound encode "hi" -o hi.wav`) next to the microphone,
and `c/build/bysound ack "hello from esp32" -o ack.wav --heard top` makes the ack the board waits for after a button press (play it a few seconds after pressing).

## Configuration notes

- Security level (menuconfig): `maximum` (default), `standard` or `basic`; `maximum` pre-listens 3-4.2 s before the first frame, so a send starts a few seconds after the button.
- Band: by default the sender uses the channel ladder; "Send on one band only" fixes it (no ladder, no calibration). `bysound bands` on the host lists all seven bands.
  The receiver always listens to top, high, upper and mid.
- Robust profile: "Also listen to the robust profile" (default on) adds a second demodulator, so both boards hear the sender's robust repetitions. Off saves about 85 KB of RAM,
  but a sender that goes robust is then not heard.
- Microphone gain: the INMP441 is quiet at 18 kHz; the default digital gain is x4 (`BYSOUND_DEMO_MIC_GAIN_SHIFT`). Raise it if nothing is heard,
  lower it if the sound clips.
- Half duplex: while the board plays a frame it does not read the microphone, and the demodulators are reset afterwards so they do not hear it. The session's listen windows
  (2 s after each repetition) and receipt listens (3 s between ack runs) are made for that. It is also why a board cannot receive while acking: the next text is heard once
  the ack loop ends (receipt or 30 s).
- The demo takes the time from `esp_timer`, the calibration goes to NVS (`bysound/cal`), and `esp_random()` jitters the pre-listen.
- CPU of two demodulators on four bands was not measured on a board.

## Footprint (default configuration, ESP-IDF 5.4.2, esp32s3)

From `idf.py size`: image 286.7 KB; internal RAM static data 150.1 KB `.bss` + 10.8 KB `.data` of 342 KB; no PSRAM needed. Of the flash, the session layer is about
10 KB, SHA-256 1 KB and NVS 13.6 KB. Of the `.bss`, the two robust-capable demodulators on the four ladder bands take 132 KB and the two sessions about 2 KB.
Without the robust receiver (`BYSOUND_DEMO_ROBUST_RX` off) a single normal-profile demodulator takes 45.5 KB, about 85 KB less.
CPU on the device was not measured (no board was used); on an Apple-silicon host the FFT engine runs a single-band demodulator at roughly 740x real time.
