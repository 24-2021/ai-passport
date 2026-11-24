#include "kfb24_config.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "kfb24_cfg";
static const char *NVS_NAMESPACE = "kfb24";

void kfb24_config_defaults(kfb24_cfg_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->port = KFB24_DEFAULT_PORT;
}

bool kfb24_config_is_wifi(const kfb24_cfg_t *cfg) {
    return cfg && cfg->ssid[0] != '\0';
}

esp_err_t kfb24_config_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建 (%s)", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    return err;
}

static void read_str(nvs_handle_t handle, const char *key, char *dst, size_t cap) {
    size_t len = cap;
    if (nvs_get_str(handle, key, dst, &len) != ESP_OK) dst[0] = '\0';
}

esp_err_t kfb24_config_load(kfb24_cfg_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    kfb24_config_defaults(out);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    read_str(handle, "ssid", out->ssid, sizeof(out->ssid));
    read_str(handle, "pass", out->pass, sizeof(out->pass));
    read_str(handle, "host", out->host, sizeof(out->host));
    uint16_t port = 0;
    if (nvs_get_u16(handle, "port", &port) == ESP_OK && port != 0) out->port = port;
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t kfb24_config_save(const kfb24_cfg_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (strlen(cfg->ssid) > KFB24_SSID_MAX || strlen(cfg->pass) > KFB24_PASS_MAX ||
        strlen(cfg->host) > KFB24_HOST_MAX || cfg->port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "ssid", cfg->ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "pass", cfg->pass);
    if (err == ESP_OK) err = nvs_set_str(handle, "host", cfg->host);
    if (err == ESP_OK) err = nvs_set_u16(handle, "port", cfg->port);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "配置已保存: wifi=%d host=%s port=%u",
                 kfb24_config_is_wifi(cfg) ? 1 : 0,
                 cfg->host[0] ? cfg->host : "-", (unsigned)cfg->port);
    }
    return err;
}
