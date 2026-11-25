// kfb24_link.h —— USB Serial/JTAG and Wi-Fi/TCP transport to the PC bridge.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "kfb24_config.h"

typedef struct {
    bool usb_connected;
    bool wifi_configured;
    bool wifi_connected;
    bool tcp_connected;
    bool ble_connected;
    bool bridge_online;
    bool bridge_usb;
    bool bridge_ble;
    char transport[8];             // USB / BLE / WiFi / -
    char ip[16];
    char ssid[KFB24_SSID_MAX + 1];
    uint32_t ack_seq;
    uint32_t ack_id;
    bool ack_ok;
} kfb24_link_status_t;

void kfb24_link_start(const kfb24_cfg_t *cfg);

// Queues one action for the link task. Returns false when no transport is up or
// the queue is full. Never blocks the LVGL task.
bool kfb24_link_send_cmd(const char *name, uint32_t id);

// Audio follows the current bridge route (USB, BLE, or Wi-Fi) and uses the same
// line framing as commands. ready() is false until a bridge is identified on
// that route; write() serializes each transport with commands and status.
bool kfb24_link_audio_ready(void);
bool kfb24_link_audio_write(const char *line, size_t len);

void kfb24_link_get_status(kfb24_link_status_t *out);
