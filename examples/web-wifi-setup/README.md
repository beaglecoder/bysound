# web-wifi-setup

A phone-side page that sends Wi-Fi credentials to a device by sound, plus a simulated device (the right half) so two tabs, or one tab with the **Loopback test (no audio)** button, are enough to try it. The device side for a real board is [`../esp32-wifi-setup`](../esp32-wifi-setup/) (ESP32-S3).

```sh
cd examples && npm run dev:web-wifi-setup
```

Format: the standard Wi-Fi QR syntax `WIFI:T:WPA;S:<ssid>;P:<password>;;` (about 50 characters for a typical network, 2 s on `mid`; protected, about 125 characters, 4 s).

**Sound is not secret.** Anyone in earshot, or with a recording, reads the plain message. The page shows the simplest honest protection: the device shows an 8-character **one-time code** on its screen (valid 2 minutes, burned after one use); the phone user types it, and the credentials travel as `BSW1|salt|iv|AES-GCM ciphertext` with a key from PBKDF2(code). A bystander without sight of the screen hears only ciphertext. Limits, spelled out in the page: about 40 bits can be brute-forced offline from a recording given enough hardware, and the phone does not authenticate the device. Use a longer code or a real pairing protocol for anything valuable.

`format.ts` is pure (Web Crypto, no DOM): the same format is what the firmware must implement (mbedtls PBKDF2 + AES-GCM).
