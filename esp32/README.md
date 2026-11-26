# bysound_demo (ESP32-S3)

Listens continuously for bysound v4 messages and logs every decoded text over serial. Press BOOT (GPIO0) to send a text (default
`hello from esp32`) three times. Uses the [`../c`](../c) library (wrapped as the `bysound` component in `components/bysound`) and the I2S standard driver
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

Expected log: `listening on band 0 ...`, then `received: <text>` for every frame heard, and `sending 1/3 ...` after a button press.
To try it without a second board, play a WAV made on the host (`c/build/bysound encode "hi" -o hi.wav`) next to the microphone.

## Configuration notes

- Band: `top` (18-19.5 kHz) is nearly inaudible; `bysound bands` on the host lists all seven. Both boards must use the same band and profile.
- Robust profile (top, high, upper, mid): slower, more noise tolerant, about 20 KB more RAM. Selecting it or the `audible` band
  turns on the matching demodulator option automatically.
- Microphone gain: the INMP441 is quiet at 18 kHz; the default digital gain is x4 (`BYSOUND_DEMO_MIC_GAIN_SHIFT`). Raise it if nothing is heard,
  lower it if the sound clips.
- The device listens on one band only (the demodulator is told to), which is cheaper than the default four.
- Transmitting pauses listening; the demodulator is reset afterwards so it does not hear itself.

## Footprint (default configuration, ESP-IDF 5.4.2, esp32s3)

From `idf.py size`: image 252 KB; internal RAM static data 59.7 KB `.bss` + 10.6 KB `.data` (of 342 KB); no PSRAM needed.
The demodulator state is about 45.5 KB of that `.bss`, the audio buffers about 9 KB, the 1 KB payload buffer and its text
buffer 3 KB. The modem code itself is about 21 KB of flash. With the robust profile `.bss` grows to about 80 KB. CPU on the
device was not measured (no board was used); on an Apple-silicon host the FFT engine runs the single-band demodulator at roughly
740x real time.
