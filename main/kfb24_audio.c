#include "kfb24_audio.h"

#include "bsp_audio.h"
#include "kfb24_adpcm.h"
#include "kfb24_link.h"
#include "kfb24_proto.h"
#include "kfb24_wake.h"

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
#define AUDIO_WAKE_COOLDOWN_MS 1200
#define AUDIO_WAKE_RETRY_MS 1000
#define PLAY_SLOTS 8
#define PLAY_TAIL_MS 600

static const char *TAG = "kfb24_audio";
static const char *B64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static TaskHandle_t s_task;
static volatile bool s_active;
static volatile bool s_stop;
static volatile uint32_t s_session;
static bool s_codec_ready;
static bool s_mic_error_logged;
static uint32_t s_wake_resume_ms;
static uint32_t s_wake_id;
static int16_t s_wake_pcm[KFB24_WAKE_MAX_SAMPLES];
static int16_t s_play_block[KFB24_ADPCM_SAMPLES];
static int16_t s_play_pcm[PLAY_SLOTS][KFB24_ADPCM_SAMPLES];
static size_t s_play_head;
static size_t s_play_tail;
static uint32_t s_play_drops;
static bool s_volume_set;
static portMUX_TYPE s_play_mux = portMUX_INITIALIZER_UNLOCKED;

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

static bool play_pop(int16_t *out) {
    bool ok = false;
    taskENTER_CRITICAL(&s_play_mux);
    if (s_play_tail != s_play_head) {
        memcpy(out, s_play_pcm[s_play_tail], sizeof(s_play_pcm[0]));
        s_play_tail = (s_play_tail + 1) % PLAY_SLOTS;
        ok = true;
    }
    taskEXIT_CRITICAL(&s_play_mux);
    return ok;
}

static bool play_empty(void) {
    bool empty;
    taskENTER_CRITICAL(&s_play_mux);
    empty = s_play_head == s_play_tail;
    taskEXIT_CRITICAL(&s_play_mux);
    return empty;
}

bool kfb24_audio_play_line(const char *line, size_t len) {
    static const char prefix[] = "KFB24 AUDIO P ";
    if (!line || len < sizeof(prefix) + 8) return false;
    if (strncmp(line, prefix, sizeof(prefix) - 1) != 0) return false;
    if (s_active) return false;  // the mic session owns the link and the codec

    unsigned long seq = 0;
    unsigned long crc = 0;
    int offset = 0;
    if (sscanf(line + sizeof(prefix) - 1, "%lu %lx %n", &seq, &crc,
               &offset) != 2 || offset <= 0) {
        return false;
    }
    size_t head = sizeof(prefix) - 1 + (size_t)offset;
    if (head >= len) return false;
    uint8_t block[KFB24_ADPCM_BYTES];
    size_t block_len = 0;
    if (!kfb24_proto_b64_decode(line + head, len - head, block, sizeof(block),
                                &block_len) ||
        block_len != KFB24_ADPCM_BYTES) {
        ESP_LOGW(TAG, "播放帧解码失败 #%lu", seq);
        return false;
    }
    if (crc32_le(block, block_len) != (uint32_t)crc) {
        ESP_LOGW(TAG, "播放块 CRC 校验失败 #%lu", seq);
        return false;
    }

    int16_t pcm[KFB24_ADPCM_SAMPLES];
    if (kfb24_adpcm_decode(block, block_len, pcm, KFB24_ADPCM_SAMPLES) !=
        KFB24_ADPCM_SAMPLES) {
        return false;
    }

    bool queued = false;
    bool idle = false;
    taskENTER_CRITICAL(&s_play_mux);
    idle = s_play_head == s_play_tail;
    size_t next = (s_play_head + 1) % PLAY_SLOTS;
    if (next != s_play_tail) {
        memcpy(s_play_pcm[s_play_head], pcm, sizeof(pcm));
        s_play_head = next;
        queued = true;
    } else {
        s_play_drops++;
    }
    taskEXIT_CRITICAL(&s_play_mux);
    if (queued && idle) ESP_LOGI(TAG, "播放开始 #%lu", seq);
    if (!queued) {
        if (s_play_drops == 1 || s_play_drops % 50 == 0) {
            ESP_LOGW(TAG, "播放缓冲已满，丢弃 %lu 块", (unsigned long)s_play_drops);
        }
    }
    return queued;
}

static void drain_mic(void) {
    static int16_t discard[KFB24_ADPCM_SAMPLES];
    for (int i = 0; i < 4; i++) {
        if (bsp_audio_read(discard, sizeof(discard)) != ESP_OK) break;
    }
}

static void send_audio_end(void) {
    static const char end[] = "KFB24 AUDIO END\n";
    (void)kfb24_link_audio_write(end, sizeof(end) - 1);
}

static esp_err_t ensure_codec(void) {
    if (s_codec_ready) return ESP_OK;
    esp_err_t err = bsp_audio_init();
    if (err == ESP_OK) {
        err = bsp_audio_set_format(AUDIO_SAMPLE_RATE, 16, 1);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "麦克风初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    s_codec_ready = true;
    return ESP_OK;
}

static void capture_once(uint32_t session) {
    static int16_t pcm[AUDIO_CHUNK_SAMPLES];
    static uint8_t adpcm[KFB24_ADPCM_BYTES];
    static char line[AUDIO_LINE_MAX];
    kfb24_adpcm_enc_t enc = {0};
    uint32_t seq = 0;

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
        // An explicit stop already closed the PC-side stream, so END only
        // reports ends the PC cannot know about: the time cap, a microphone
        // read error, or a transport failure. The bridge releases the latched
        // WeType hotkey on END instead of leaving it listening to silence.
        bool board_ended = !s_stop;
        if (board_ended) send_audio_end();
        ESP_LOGI(TAG, "%s", board_ended ? "录音异常结束" : "录音结束");
    }
}

// Always-on wake word detection. This is the only mic reader, so streaming and
// detection can never split I2S frames: the same task switches modes.
static void listen_for_wake(void) {
    int chunk = kfb24_wake_chunk_samples();
    if (chunk <= 0) return;
    if (bsp_audio_read(s_wake_pcm, (size_t)chunk * sizeof(int16_t)) != ESP_OK) {
        if (!s_mic_error_logged) {
            ESP_LOGW(TAG, "唤醒麦克风读取失败");
            s_mic_error_logged = true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        return;
    }
    s_mic_error_logged = false;

    if (!kfb24_wake_feed(s_wake_pcm)) return;

    uint32_t id = ++s_wake_id;
    if (!kfb24_link_send_cmd("wake", id)) {
        ESP_LOGW(TAG, "唤醒命中，但电脑桥接未连接");
        s_wake_resume_ms = (uint32_t)(esp_timer_get_time() / 1000) +
                           AUDIO_WAKE_RETRY_MS;
        return;
    }

    ESP_LOGI(TAG, "语音唤醒命中 #%lu，开始上行", (unsigned long)id);
    s_stop = false;
    s_active = true;
    s_session++;
}

static void audio_task(void *arg) {
    (void)arg;
    if (ensure_codec() != ESP_OK) {
        ESP_LOGW(TAG, "麦克风未就绪，语音唤醒与按键录音暂不可用");
    }
    if (kfb24_wake_init() != ESP_OK) {
        ESP_LOGW(TAG, "语音唤醒不可用，仍可使用按键语音输入");
    }

    for (;;) {
        if (s_active) {
            if (ensure_codec() != ESP_OK) {
                s_active = false;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            capture_once(s_session);
            s_active = false;
            s_stop = false;
            s_wake_resume_ms = (uint32_t)(esp_timer_get_time() / 1000) +
                               AUDIO_WAKE_COOLDOWN_MS;
            continue;
        }

        if (play_pop(s_play_block)) {
            if (ensure_codec() == ESP_OK) {
                if (!s_volume_set) {
                    bsp_audio_set_volume(88);
                    s_volume_set = true;
                }
                bsp_audio_write(s_play_block, sizeof(s_play_block));
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (play_empty()) {
                // Keep the wake detector quiet through the codec tail; the
                // TTS voice itself can otherwise match the wake word.
                s_wake_resume_ms =
                    (uint32_t)(esp_timer_get_time() / 1000) + PLAY_TAIL_MS;
                if (kfb24_wake_ready()) {
                    drain_mic();
                }
                ESP_LOGI(TAG, "播放结束");
            }
            continue;
        }

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (kfb24_wake_ready() && kfb24_link_audio_ready() &&
            (int32_t)(now - s_wake_resume_ms) >= 0) {
            listen_for_wake();
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void kfb24_audio_init(void) {
    if (s_task) return;
    if (xTaskCreate(audio_task, "kfb24_audio", 4096, NULL, 6, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "录音任务创建失败");
        s_task = NULL;
    }
}

bool kfb24_audio_start(void) {
    if (s_active) return true;
    kfb24_audio_init();
    if (!s_task) return false;
    if (!kfb24_link_audio_ready()) return false;
    s_stop = false;
    s_active = true;
    s_session++;
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
