/* I2S microphone in, I2S amplifier out, and the bysound modem and session layer in between. */
#pragma once
#include "esp_err.h"

/* Called once a text was received AND confirmed (the confirm gate): `text` is valid only during the call. */
typedef void (*audio_text_cb_t)(const char *text);

/* Starts the I2S ports and the audio task. The board listens continuously as a session receiver: it delivers the text, acknowledges it by
 * sound until the sender's receipt, then listens again. */
esp_err_t audio_start(audio_text_cb_t on_text);

/* Sends `text` with the session sender: pre-listen, channel ladder, stops when acked (the log says which band worked, the calibration
 * is kept in NVS). Returns ESP_ERR_INVALID_STATE if a send is already in progress. */
esp_err_t audio_send(const char *text);
