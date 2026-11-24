// kfb24_config.h —— Wi-Fi and PC bridge settings persisted in NVS.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "kfb24_proto.h"

typedef struct {
    char ssid[KFB24_SSID_MAX + 1];
    char pass[KFB24_PASS_MAX + 1];
    char host[KFB24_HOST_MAX + 1];
    uint16_t port;
} kfb24_cfg_t;

#define KFB24_DEFAULT_PORT 8765

void kfb24_config_defaults(kfb24_cfg_t *out);
bool kfb24_config_is_wifi(const kfb24_cfg_t *cfg);

// Initializes NVS once; erases it only on version/free-page incompatibility.
esp_err_t kfb24_config_init(void);
esp_err_t kfb24_config_load(kfb24_cfg_t *out);
esp_err_t kfb24_config_save(const kfb24_cfg_t *cfg);
