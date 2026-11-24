#include "kfb24_power.h"

#include "bsp_battery.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kfb24_pwr";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_soc = -1;
static int s_mv = -1;
static bool s_started;

static void publish(int soc, int mv) {
    taskENTER_CRITICAL(&s_lock);
    s_soc = soc;
    s_mv = mv;
    taskEXIT_CRITICAL(&s_lock);
}

static void power_task(void *arg) {
    (void)arg;
    for (;;) {
        esp_err_t err = bsp_battery_init();
        if (err != ESP_OK) {
            publish(-1, -1);
            ESP_LOGW(TAG, "电量计不可用: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }
        publish(bsp_battery_soc(), bsp_battery_mv());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void kfb24_power_start(void) {
    if (s_started) return;
    s_started = true;
    if (xTaskCreate(power_task, "kfb24_power", 4096, NULL, 2, NULL) != pdPASS) {
        s_started = false;
        ESP_LOGE(TAG, "电量任务创建失败");
    }
}

void kfb24_power_get(int *soc, int *mv) {
    taskENTER_CRITICAL(&s_lock);
    int soc_value = s_soc;
    int mv_value = s_mv;
    taskEXIT_CRITICAL(&s_lock);
    if (soc) *soc = soc_value;
    if (mv) *mv = mv_value;
}
