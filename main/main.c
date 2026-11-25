// kfb24 —— a PC command console for the FoloToy AI Passport board.
// Three buttons drive eight actions over USB, Wi-Fi, or BLE through the
// kfb24 PC bridge.
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kfb24_config.h"
#include "kfb24_link.h"
#include "kfb24_power.h"
#include "kfb24_ui.h"

static const char *TAG = "kfb24";

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} input_event_t;

static QueueHandle_t s_input_q;

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_q) return;
    const input_event_t event = {.btn = btn, .ev = ev};
    (void)xQueueSend(s_input_q, &event, 0);
}

static void input_task(void *arg) {
    (void)arg;
    for (;;) {
        input_event_t event;
        if (xQueueReceive(s_input_q, &event, portMAX_DELAY) == pdTRUE) {
            kfb24_ui_handle_input(event.btn, event.ev);
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "kfb24 1.0.2 by 24 启动");

    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "I2C 初始化失败: %s", esp_err_to_name(err));

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败，无法启动 UI");
        return;
    }
    bsp_display_backlight(100);

    err = kfb24_config_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
    }
    kfb24_cfg_t cfg;
    kfb24_config_defaults(&cfg);
    if (kfb24_config_load(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "配置读取失败，使用默认值");
    }

    s_input_q = xQueueCreate(16, sizeof(input_event_t));
    if (!s_input_q) {
        ESP_LOGE(TAG, "按键队列创建失败");
    } else if (xTaskCreate(input_task, "kfb24_input", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "按键任务创建失败");
    } else if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
    }

    kfb24_link_start(&cfg);
    kfb24_power_start();
    kfb24_ui_start();

    ESP_LOGI(TAG, "就绪: Wi-Fi %s，USB/BLE 控制台可用",
             kfb24_config_is_wifi(&cfg) ? "已配置" : "未配置");
}
