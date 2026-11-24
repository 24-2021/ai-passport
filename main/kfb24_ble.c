#include "kfb24_ble.h"

#include "kfb24_proto.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "kfb24_ble";

// Nordic UART Service, compatible with common BLE serial tools.
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

#define BLE_RX_QUEUE_LEN 4
#define BLE_LINE_MAX KFB24_PROTO_MAX_LINE

static QueueHandle_t s_rx_q;
static char s_line[BLE_LINE_MAX];
static size_t s_line_len;
static bool s_line_overflow;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_rx_handle;
static uint16_t s_tx_handle;
static uint8_t s_own_addr_type;
static bool s_started;

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg);
static int gap_event(struct ble_gap_event *event, void *arg);
static void advertise(void);

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // PC -> board. Write-only, with or without a response.
                .uuid = &s_rx_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_rx_handle,
            },
            {
                // Board -> PC notifications.
                .uuid = &s_tx_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_handle,
            },
            {0},
        },
    },
    {0},
};

static void queue_line(void) {
    if (s_line_len == 0) return;
    s_line[s_line_len] = '\0';
    if (s_rx_q && xQueueSend(s_rx_q, s_line, 0) != pdTRUE) {
        ESP_LOGW(TAG, "BLE 接收队列已满，丢弃: %s", s_line);
    }
    s_line_len = 0;
    s_line_overflow = false;
}

static void receive_bytes(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n') {
            if (s_line_overflow) {
                ESP_LOGW(TAG, "BLE 行过长，已丢弃");
                s_line_len = 0;
                s_line_overflow = false;
            } else {
                queue_line();
            }
        } else if (c != '\r') {
            if (s_line_len + 1 < sizeof(s_line) && !s_line_overflow) {
                s_line[s_line_len++] = c;
            } else {
                s_line_overflow = true;
            }
        }
    }
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
        attr_handle == s_rx_handle) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t chunk[256];
        if (len > sizeof(chunk)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint16_t copied = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, chunk, sizeof(chunk), &copied);
        if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
        receive_bytes(chunk, copied);
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR &&
        attr_handle == s_tx_handle) {
        // The TX characteristic is notify-only; report an empty value for
        // clients that probe it.
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }

    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static void advertise(void) {
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = (uint8_t)strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE 广播数据失败: %d", rc);
        return;
    }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params,
                           gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "BLE 广播启动失败: %d", rc);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE 已连接, handle=%u",
                     (unsigned)s_conn_handle);
            ble_gap_adv_stop();
        } else {
            ESP_LOGW(TAG, "BLE 连接失败: %d", event->connect.status);
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE 已断开: %d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "BLE 通知订阅: %s",
                 event->subscribe.cur_notify ? "on" : "off");
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE MTU: %u", (unsigned)event->mtu.value);
        return 0;
    default:
        return 0;
    }
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "BLE 重置: %d", reason);
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

static int use_random_address(void) {
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_BT);
    if (err != ESP_OK) return -1;

    // Stable random static identity, derived from the chip MAC. The two most
    // significant bits of the last byte mark a static random address (BLE is
    // little-endian), avoiding collisions with a public identity left behind
    // by a previous firmware on the same chip.
    uint8_t rnd[6];
    for (size_t i = 0; i < sizeof(rnd); i++) rnd[i] = mac[i] ^ 0x5a;
    rnd[5] = (uint8_t)((rnd[5] & 0x3f) | 0xc0);

    int rc = ble_hs_id_set_rnd(rnd);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE 随机地址设置失败: %d", rc);
        return rc;
    }
    s_own_addr_type = BLE_OWN_ADDR_RANDOM;
    ESP_LOGI(TAG, "BLE 地址: %02x:%02x:%02x:%02x:%02x:%02x",
             rnd[5], rnd[4], rnd[3], rnd[2], rnd[1], rnd[0]);
    return 0;
}

static void on_sync(void) {
    if (use_random_address() != 0) {
        int rc = ble_hs_util_ensure_addr(0);
        if (rc != 0) {
            ESP_LOGE(TAG, "BLE 地址初始化失败: %d", rc);
            return;
        }
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
        if (rc != 0) {
            ESP_LOGE(TAG, "BLE 地址类型推断失败: %d", rc);
            return;
        }
    }
    advertise();
    ESP_LOGI(TAG, "BLE 广播中: kfb24");
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t kfb24_ble_start(void) {
    if (s_started) return ESP_OK;

    s_rx_q = xQueueCreate(BLE_RX_QUEUE_LEN, BLE_LINE_MAX);
    if (!s_rx_q) return ESP_ERR_NO_MEM;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set("kfb24");
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE 设备名设置失败: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE GATT 注册失败: %d", rc);
        return ESP_FAIL;
    }

    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    nimble_port_freertos_init(host_task);
    s_started = true;
    return ESP_OK;
}

bool kfb24_ble_connected(void) {
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool kfb24_ble_send(const char *data, size_t len) {
    if (!data || len == 0) return true;
    uint16_t conn = s_conn_handle;
    if (conn == BLE_HS_CONN_HANDLE_NONE || s_tx_handle == 0) return false;

    uint16_t mtu = ble_att_mtu(conn);
    if (mtu < 23) mtu = 23;
    size_t chunk_size = (size_t)mtu - 3;
    if (chunk_size > 244) chunk_size = 244;

    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > chunk_size) n = chunk_size;

        int rc = BLE_HS_ENOMEM;
        for (int attempt = 0; attempt < 3; attempt++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(data + off, (uint16_t)n);
            if (!om) {
                vTaskDelay(pdMS_TO_TICKS(4));
                continue;
            }
            rc = ble_gatts_notify_custom(conn, s_tx_handle, om);
            if (rc == 0) break;
            vTaskDelay(pdMS_TO_TICKS(4));
        }
        if (rc != 0) {
            ESP_LOGW(TAG, "BLE 通知失败: %d", rc);
            return false;
        }
        off += n;
        if (off < len) vTaskDelay(pdMS_TO_TICKS(4));
    }
    return true;
}

bool kfb24_ble_read_line(char *buf, size_t cap) {
    if (!buf || cap == 0 || !s_rx_q) return false;
    char line[BLE_LINE_MAX];
    if (xQueueReceive(s_rx_q, line, 0) != pdTRUE) return false;
    snprintf(buf, cap, "%s", line);
    return true;
}
