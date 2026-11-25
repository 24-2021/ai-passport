#include "kfb24_audio.h"

#include "bsp_audio.h"
#include "kfb24_adpcm.h"
#include "kfb24_link.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define AUDIO_SAMPLE_RATE KFB24_ADPCM_SAMPLE_RATE
#define AUDIO_CHUNK_SAMPLES KFB24_ADPCM_SAMPLES
#define AUDIO_CHUNK_BYTES (AUDIO_CHUNK_SAMPLES * sizeof(int16_t))
#define AUDIO_MAX_MS 60000
#define AUDIO_LINE_MAX 512
#define AUDIO_CMD_START 1

static const char *TAG = "kfb24_audio";
static const char *B64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static TaskHandle_t s_task;
static volatile bool s_active;
static volatile bool s_stop;
static volatile uint32_t s_session;
static bool s_codec_ready;

static uint32_t crc32_le(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

static size_t base64_encode(const uint8_t *src, size_t len, char *out,
                            size_t cap) {
    size_t o = 0;
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        if (o + 4 >= cap) return 0;
        uint32_t v = ((uint32_t)src[i] << 16) |
                     ((uint32_t)src[i + 1] << 8) | src[i + 2];
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = B64[(v >> 6) & 0x3F];
        out[o++] = B64[v & 0x3F];
    }
    if (i < len) {
        if (o + 4 >= cap) return 0;
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1 < len) v |= (uint32_t)src[i + 1] << 8;
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? B64[(v >> 6) & 0x3F] : '=';
        out[o++] = '=';
    }
    if (o >= cap) return 0;
    out[o] = '\0';
    return o;
}

static void send_audio_end(void) {
    static const char end[] = "KFB24 AUDIO END\n";
    (void)kfb24_link_audio_write(end, sizeof(end) - 1);
}

static void capture_once(uint32_t session) {
    static int16_t pcm[AUDIO_CHUNK_SAMPLES];
    static uint8_t adpcm[KFB24_ADPCM_BYTES];
    static char line[AUDIO_LINE_MAX];
    kfb24_adpcm_enc_t enc = {0};
    uint32_t seq = 0;

    if (!s_codec_ready) {
        esp_err_t err = bsp_audio_init();
        if (err == ESP_OK) {
            err = bsp_audio_set_format(AUDIO_SAMPLE_RATE, 16, 1);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "麦克风初始化失败: %s", esp_err_to_name(err));
            s_active = false;
            return;
        }
        s_codec_ready = true;
    }

    int64_t started = esp_timer_get_time();
    ESP_LOGI(TAG, "录音开始 (ADPCM %d Hz, %d ms/block)", AUDIO_SAMPLE_RATE,
             (int)(AUDIO_CHUNK_SAMPLES * 1000 / AUDIO_SAMPLE_RATE));
    uint32_t blocks = 0;
    while (!s_stop && s_session == session) {
        if ((esp_timer_get_time() - started) >= (int64_t)AUDIO_MAX_MS * 1000) {
            ESP_LOGI(TAG, "录音达到 %d 秒上限", AUDIO_MAX_MS / 1000);
            break;
        }
        if (bsp_audio_read(pcm, AUDIO_CHUNK_BYTES) != ESP_OK) {
            ESP_LOGW(TAG, "读取麦克风失败，停止录音");
            break;
        }
        // 诊断: 每秒打印一次原始 PCM 峰值, 判断 codec 是否真的采到声音。
        int peak = 0;
        for (int i = 0; i < AUDIO_CHUNK_SAMPLES; i++) {
            int v = pcm[i];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
        if (++blocks == 1 || blocks % 25 == 0) {
            ESP_LOGI(TAG, "麦克风电平 peak=%d", peak);
        }
        size_t encoded = kfb24_adpcm_encode(pcm, AUDIO_CHUNK_SAMPLES, adpcm,
                                            sizeof(adpcm), &enc);
        if (encoded == 0) break;
        uint32_t crc = crc32_le(adpcm, encoded);
        int prefix = snprintf(line, sizeof(line), "KFB24 AUDIO A %lu %08lx ",
                              (unsigned long)seq, (unsigned long)crc);
        if (prefix <= 0 || (size_t)prefix + 2 >= sizeof(line)) break;
        size_t b64 = base64_encode(adpcm, encoded, line + prefix,
                                   sizeof(line) - (size_t)prefix - 1);
        if (b64 == 0) break;
        size_t len = (size_t)prefix + b64;
        line[len++] = '\n';
        if (!kfb24_link_audio_write(line, len)) {
            ESP_LOGW(TAG, "音频发送失败，停止录音");
            break;
        }
        seq++;
    }

    if (s_session == session) {
        s_active = false;
        // An explicit stop already closed the PC-side stream, so END only
        // reports ends the PC cannot know about: the time cap, a microphone
        // read error, or a transport failure. The bridge releases the latched
        // WeType hotkey on END instead of leaving it listening to silence.
        bool board_ended = !s_stop;
        s_stop = false;
        if (board_ended) send_audio_end();
        ESP_LOGI(TAG, "%s", board_ended ? "录音异常结束" : "录音结束");
    }
}

static void audio_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t command = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (command == AUDIO_CMD_START) {
            capture_once(s_session);
        }
    }
}

bool kfb24_audio_start(void) {
    if (s_active) return true;
    if (!kfb24_link_audio_ready()) return false;
    if (!s_task) {
        if (xTaskCreate(audio_task, "kfb24_audio", 4096, NULL, 6, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "录音任务创建失败");
            s_task = NULL;
            return false;
        }
    }
    s_stop = false;
    s_active = true;
    s_session++;
    xTaskNotify(s_task, AUDIO_CMD_START, eSetValueWithOverwrite);
    return true;
}

void kfb24_audio_stop(void) {
    if (!s_active) return;
    s_stop = true;
    s_active = false;
}

bool kfb24_audio_is_active(void) {
    return s_active;
}
