// kfb24_audio.h —— board microphone capture for the PC voice action.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Starts the microphone worker: loads the WakeNet model and listens for the
// wake word whenever a bridge is connected. Call once after kfb24_link_start().
void kfb24_audio_init(void);

// Starts streaming 16 kHz / 16-bit / mono PCM chunks to the PC bridge over USB.
// Returns false when USB is not the active bridge route. The capture itself runs
// in its own task; this never blocks the LVGL/button task.
bool kfb24_audio_start(void);

// Requests the current capture to stop. Safe to call when idle.
void kfb24_audio_stop(void);

// True while the capture task is streaming (or still unwinding).
bool kfb24_audio_is_active(void);

// Queues one PC -> board playback block ("KFB24 AUDIO P <seq> <crc> <b64>",
// 640-sample IMA ADPCM). Called from the link task and never blocks; the audio
// task plays the block and suppresses wake detection until the tail is gone.
bool kfb24_audio_play_line(const char *line, size_t len);

// Marks the end of a playback stream so the screen can stop showing "speaking"
// and the wake word resumes after the tail delay.
void kfb24_audio_play_end(void);
