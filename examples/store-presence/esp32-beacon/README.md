# store-presence / esp32-beacon

ESP-IDF project (ESP32-S3) that plays the presence token of [`../README.md`](../README.md) on the `top` channel every few seconds. Transmit only: no microphone, no session, no acknowledgements. It reuses the `bysound` component (`esp32/components/bysound`, the C library in `c/`) and the speaker pins and output level from `esp32/main/Kconfig.projbuild`; the token is computed with mbedTLS (`mbedtls_md_hmac`, SHA-256) and matches `../token.ts`.

Hardware: an I2S amplifier (MAX98357A) with a speaker, wired as in `esp32/README.md` (the microphone is not used here).

## Build

```sh
. ~/esp/esp-idf/export.sh                 # ESP-IDF 5.4
cd examples/store-presence/esp32-beacon
idf.py set-target esp32s3
idf.py menuconfig                         # "Store presence beacon": store id, key, interval, time source
idf.py build                              # then flash with idf.py -p PORT flash monitor
```

## Time

The token needs the unix time (the window is `floor(unix_time / 30)`), and the customer rejects windows more than one away from its own.

- **SNTP** (`PRESENCE_TIME_SNTP`): the board joins the Wi-Fi network you configure and syncs with `pool.ntp.org` (or your server) before it starts. Use this for anything real.
- **Fixed epoch** (default, `PRESENCE_TIME_FIXED`): offline demo. The clock starts at `PRESENCE_EPOCH` when the board boots and counts uptime. Set the epoch to the current time right before building (`date +%s`) and power the board soon after; if the boot is more than about a minute after that time, customers see "too old". It is only for trying the sound without a network.

## Key

`PRESENCE_STORE_KEY` defaults to the same demo secret as the web pages so the two interoperate out of the box. A real store gets a random key kept in its backend and provisioned into the device's NVS or a protected partition, not in `sdkconfig`.

## Status

Builds (ESP-IDF 5.4, ESP32-S3, with both time sources, image about 250 KB fixed-epoch and 760 KB with Wi-Fi). Not flashed or heard on a board.
