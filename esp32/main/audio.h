/* I2S microphone in, I2S amplifier out, and the bysound modem in between. */
#pragma once
#include "esp_err.h"

/* Receive callback: `text` is valid only during the call. */
typedef void (*audio_text_cb_t)(const char *text);

/* Starts the I2S ports and the audio task, which listens continuously and logs/forwards every decoded text. */
esp_err_t audio_start(audio_text_cb_t on_text);

/* Queues `text` to be sent `repeat` times (the modem pauses listening meanwhile). Returns ESP_ERR_INVALID_STATE if busy. */
esp_err_t audio_send(const char *text, int repeat);
