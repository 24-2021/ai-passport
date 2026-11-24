// kfb24_audio.h —— board microphone capture for the PC voice action.
#pragma once

#include <stdbool.h>

// Starts streaming 16 kHz / 16-bit / mono PCM chunks to the PC bridge over USB.
// Returns false when USB is not the active bridge route. The capture itself runs
// in its own task; this never blocks the LVGL/button task.
bool kfb24_audio_start(void);

// Requests the current capture to stop. Safe to call when idle.
void kfb24_audio_stop(void);

// True while the capture task is streaming (or still unwinding).
bool kfb24_audio_is_active(void);
