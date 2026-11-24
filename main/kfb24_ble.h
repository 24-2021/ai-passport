// kfb24_ble.h —— Nordic UART Service (NUS) peripheral for PC bridge traffic.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Starts the NimBLE host and advertises as "kfb24". Safe to call once.
esp_err_t kfb24_ble_start(void);

// True while a central is connected (PC bridge or a diagnostic tool).
bool kfb24_ble_connected(void);

// Sends one protocol line (usually LF-terminated). Fragments to the negotiated
// ATT MTU. Returns false when disconnected or the notification queue is full.
bool kfb24_ble_send(const char *data, size_t len);

// Pops one complete line received on the NUS RX characteristic. Non-blocking.
bool kfb24_ble_read_line(char *buf, size_t cap);
