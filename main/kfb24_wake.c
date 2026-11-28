#include "kfb24_wake.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include <stddef.h>

static const char *TAG = "kfb24_wake";

static srmodel_list_t *s_models;
static const esp_wn_iface_t *s_iface;
static model_iface_data_t *s_model;
static int s_chunk;

esp_err_t kfb24_wake_init(void) {
    if (s_model) return ESP_OK;

    s_models = esp_srmodel_init("model");
    if (!s_models || s_models->num <= 0) {
        ESP_LOGW(TAG, "未找到 model 分区，语音唤醒不可用");
        return ESP_ERR_NOT_FOUND;
    }

    char *name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, "nihaoxiaozhi");
    if (!name) {
        ESP_LOGW(TAG, "model 分区里没有你好小智唤醒模型");
        return ESP_ERR_NOT_FOUND;
    }

    s_iface = esp_wn_handle_from_name(name);
    if (!s_iface) {
        ESP_LOGW(TAG, "唤醒模型句柄创建失败: %s", name);
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_model = s_iface->create(name, DET_MODE_90);
    if (!s_model) {
        ESP_LOGE(TAG, "唤醒模型内存不足: %s（free=%u）", name,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }

    s_chunk = s_iface->get_samp_chunksize(s_model);
    if (s_chunk <= 0 || s_chunk > KFB24_WAKE_MAX_SAMPLES) {
        ESP_LOGE(TAG, "唤醒帧长度异常: %d", s_chunk);
        s_iface->destroy(s_model);
        s_model = NULL;
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "唤醒词就绪: %s, %d 样本/帧, free=%u", name, s_chunk,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return ESP_OK;
}

bool kfb24_wake_ready(void) {
    return s_model != NULL;
}

int kfb24_wake_chunk_samples(void) {
    return s_model ? s_chunk : 0;
}

bool kfb24_wake_feed(int16_t *pcm) {
    if (!s_model || !pcm) return false;
    return s_iface->detect(s_model, pcm) == WAKENET_DETECTED;
}
