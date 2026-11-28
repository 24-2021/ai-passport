// kfb24_wake.h —— on-device wake word ("你好小智", WakeNet9s).
//
// ESP32-C3 has no PSRAM and no SIMD, so only the WakeNet9s/10s models are
// usable. The model is mapped from the `model` flash partition by esp-sr; no
// audio front end (AEC/VAD) is available on this chip, so the caller must feed
// raw 16 kHz mono PCM and must not play audio while listening.
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Upper bound for a WakeNet frame (30 ms at 16 kHz = 480 samples).
#define KFB24_WAKE_MAX_SAMPLES 512

// Loads the model from the `model` partition and creates the detector.
// Returns ESP_ERR_NOT_FOUND when the partition or model is missing; callers
// must keep button control working in that case.
esp_err_t kfb24_wake_init(void);

bool kfb24_wake_ready(void);

// Frame size in samples; valid only when ready() is true.
int kfb24_wake_chunk_samples(void);

// Feeds exactly chunk_samples(); returns true once per detection.
bool kfb24_wake_feed(int16_t *pcm);
