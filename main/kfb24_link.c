#include "kfb24_link.h"

#include "kfb24_config.h"
#include "kfb24_proto.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#define BRIDGE_TIMEOUT_MS 8000
#define USB_POLL_MS 500
#define WIFI_RETRY_MS 5000
#define TCP_RETRY_MS 3000
#define USB_LINE_MAX KFB24_PROTO_MAX_LINE

static const char *TAG = "kfb24_link";
static const char *FW_VERSION = "kfb24/1.0.0";

typedef struct {
    char name[16];
    uint32_t id;
} cmd_item_t;

typedef enum {
    NET_EVT_START = 0,
    NET_EVT_DISCONNECTED,
    NET_EVT_GOT_IP,
} net_evt_type_t;

typedef struct {
    net_evt_type_t type;
    uint32_t ip;
} net_evt_t;

static kfb24_cfg_t s_cfg;
static SemaphoreHandle_t s_status_lock;
static kfb24_link_status_t s_status;
static QueueHandle_t s_cmd_q;

static bool s_usb_driver;
static bool s_usb_connected;
static uint32_t s_usb_poll_ms;

static bool s_wifi_inited;
static bool s_wifi_started;
static bool s_wifi_connected;
static esp_netif_t *s_netif;
static QueueHandle_t s_net_q;
static char s_ip[16];
static uint32_t s_wifi_retry_ms;
static uint8_t s_wifi_fail_logs;

static int s_fd = -1;
static uint32_t s_tcp_retry_ms;
static bool s_tcp_fail_logged;

static uint32_t s_last_hello_ms;
static bool s_bridge_usb;
static bool s_bridge_online;

static char s_usb_line[USB_LINE_MAX];
static size_t s_usb_line_len;
static char s_tcp_line[USB_LINE_MAX];
static size_t s_tcp_line_len;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void status_ack(uint32_t id, bool ok) {
    if (!s_status_lock) return;
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.ack_seq++;
    s_status.ack_id = id;
    s_status.ack_ok = ok;
    xSemaphoreGive(s_status_lock);
}

void kfb24_link_get_status(kfb24_link_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_status_lock) return;
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_lock);
}

static bool usb_write(const char *line, size_t len) {
    if (!s_usb_driver || !s_usb_connected) return false;
    int written = usb_serial_jtag_write_bytes(line, len, pdMS_TO_TICKS(100));
    return written == (int)len;
}

static void tcp_close(void) {
    if (s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
        ESP_LOGI(TAG, "TCP 已断开");
    }
}

static bool tcp_write(const char *line, size_t len) {
    if (s_fd < 0) return false;
    size_t off = 0;
    int waits = 0;
    while (off < len) {
        int n = send(s_fd, line + off, len - off, 0);
        if (n > 0) {
            off += (size_t)n;
            waits = 0;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waits < 20) {
            waits++;
            vTaskDelay(pdMS_TO_TICKS(5));
        } else {
            ESP_LOGW(TAG, "TCP 发送失败: errno=%d", errno);
            tcp_close();
            return false;
        }
    }
    return true;
}

static bool send_line_to_pc(const char *line, size_t len) {
    bool sent = false;
    if (s_usb_connected && (s_bridge_usb || s_fd < 0)) {
        sent = usb_write(line, len);
    }
    if (!sent && s_fd >= 0) {
        sent = tcp_write(line, len);
    }
    if (!sent && s_usb_connected) {
        sent = usb_write(line, len);
    }
    return sent;
}

static void send_ack(uint32_t id, bool ok) {
    char line[64];
    size_t len = kfb24_proto_fmt_ack(line, sizeof(line), id, ok);
    if (len) send_line_to_pc(line, len);
}

static void send_hello(void) {
    char line[64];
    size_t len = kfb24_proto_fmt_hello(line, sizeof(line), FW_VERSION);
    if (!len) return;
    if (s_usb_connected) usb_write(line, len);
    if (s_fd >= 0) tcp_write(line, len);
}

static void send_state(bool force) {
    static bool last_usb_up = false;
    static bool last_wifi_up = false;
    static uint32_t last_sent_ms = 0;
    uint32_t now = now_ms();

    bool usb_up = s_usb_connected;
    bool wifi_up = s_fd >= 0;
    if (!force && usb_up == last_usb_up && wifi_up == last_wifi_up &&
        now - last_sent_ms < 5000) {
        return;
    }
    last_usb_up = usb_up;
    last_wifi_up = wifi_up;
    last_sent_ms = now;

    char line[96];
    if (usb_up) {
        size_t len = kfb24_proto_fmt_state(line, sizeof(line), "usb", true,
                                           s_bridge_usb ? "bridge" : "console");
        if (len) usb_write(line, len);
    }
    if (wifi_up) {
        size_t len = kfb24_proto_fmt_state(line, sizeof(line), "wifi", true,
                                           s_ip[0] ? s_ip : "tcp");
        if (len) tcp_write(line, len);
    }
}

static void net_event_handler(void *arg, esp_event_base_t base, int32_t id,
                              void *data);

static esp_err_t wifi_init_once(void) {
    if (s_wifi_inited) return ESP_OK;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) return ESP_FAIL;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) return err;

    if (!s_net_q) return ESP_FAIL;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     net_event_handler, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         net_event_handler, NULL);
    }
    if (err != ESP_OK) return err;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (err != ESP_OK) return err;

    s_wifi_inited = true;
    return ESP_OK;
}

static esp_err_t wifi_apply(const kfb24_cfg_t *cfg) {
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }
    tcp_close();
    s_wifi_connected = false;
    s_ip[0] = '\0';
    s_wifi_fail_logs = 0;

    if (!cfg->ssid[0]) {
        ESP_LOGI(TAG, "Wi-Fi 未配置，仅使用 USB");
        return ESP_OK;
    }
    if (!s_wifi_inited) {
        esp_err_t err = wifi_init_once();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi 初始化失败: %s", esp_err_to_name(err));
            return err;
        }
    }

    wifi_config_t wc = {0};
    size_t ssid_len = strlen(cfg->ssid);
    size_t pass_len = strlen(cfg->pass);
    memcpy(wc.sta.ssid, cfg->ssid, ssid_len);
    memcpy(wc.sta.password, cfg->pass, pass_len);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 启动失败: %s", esp_err_to_name(err));
        return err;
    }
    s_wifi_started = true;
    s_wifi_retry_ms = now_ms() + WIFI_RETRY_MS;
    ESP_LOGI(TAG, "Wi-Fi 已启动，连接中 (bridge=%s:%u)",
             cfg->host[0] ? cfg->host : "-", (unsigned)cfg->port);
    return ESP_OK;
}

static void tcp_try_connect(void) {
    if (!s_wifi_connected || !s_cfg.host[0] || s_fd >= 0) return;
    uint32_t now = now_ms();
    if ((int32_t)(now - s_tcp_retry_ms) < 0) return;
    s_tcp_retry_ms = now + TCP_RETRY_MS;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(s_cfg.port);
    if (inet_pton(AF_INET, s_cfg.host, &dest.sin_addr) != 1) {
        if (!s_tcp_fail_logged) {
            ESP_LOGE(TAG, "桥接地址不是 IPv4: %s", s_cfg.host);
            s_tcp_fail_logged = true;
        }
        s_tcp_retry_ms = now + 30000;
        return;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGW(TAG, "socket 失败: errno=%d", errno);
        return;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, (struct sockaddr *)&dest, sizeof(dest));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return;
    }
    if (rc < 0) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(fd, &write_set);
        struct timeval tv = {.tv_sec = 1, .tv_usec = 500000};
        rc = select(fd + 1, NULL, &write_set, NULL, &tv);
        if (rc <= 0) {
            close(fd);
            return;
        }
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0 ||
            so_error != 0) {
            close(fd);
            return;
        }
    }

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    s_fd = fd;
    s_tcp_fail_logged = false;
    s_bridge_usb = false;
    s_last_hello_ms = 0;
    s_bridge_online = false;
    ESP_LOGI(TAG, "TCP 已连接 %s:%u", s_cfg.host, (unsigned)s_cfg.port);
    send_hello();
    send_state(true);
}

static void feed_line(char *buf, size_t *len, const char *data, size_t count,
                      bool via_usb);

static void handle_cfg(const kfb24_msg_t *msg) {
    kfb24_cfg_t cfg = {0};
    memcpy(cfg.ssid, msg->ssid, sizeof(cfg.ssid));
    memcpy(cfg.pass, msg->pass, sizeof(cfg.pass));
    memcpy(cfg.host, msg->host, sizeof(cfg.host));
    cfg.port = msg->port;

    esp_err_t err = kfb24_config_save(&cfg);
    if (err == ESP_OK) {
        s_cfg = cfg;
        s_tcp_fail_logged = false;
        err = wifi_apply(&cfg);
    }
    send_ack(msg->id, err == ESP_OK);
}

static void handle_line(const char *line, bool via_usb) {
    kfb24_msg_t msg;
    if (!kfb24_proto_parse(line, &msg)) return;

    switch (msg.type) {
    case KFB24_MSG_HELLO:
        s_last_hello_ms = now_ms();
        s_bridge_usb = via_usb;
        s_bridge_online = true;
        if (!via_usb) s_tcp_fail_logged = false;
        send_state(true);
        break;
    case KFB24_MSG_ACK:
        status_ack(msg.id, msg.ok);
        break;
    case KFB24_MSG_CFG:
        if (!via_usb) {
            ESP_LOGW(TAG, "拒绝 LAN 配置请求");
            send_ack(msg.id, false);
        } else {
            handle_cfg(&msg);
        }
        break;
    default:
        break;
    }
}

static void feed_line(char *buf, size_t *len, const char *data, size_t count,
                      bool via_usb) {
    for (size_t i = 0; i < count; i++) {
        char c = data[i];
        if (c == '\n') {
            buf[*len] = '\0';
            if (*len > 0) handle_line(buf, via_usb);
            *len = 0;
        } else if (c != '\r') {
            if (*len + 1 < USB_LINE_MAX) {
                buf[(*len)++] = c;
            } else {
                *len = 0;
            }
        }
    }
}

static void usb_rx(void) {
    if (!s_usb_driver) return;
    char tmp[256];
    for (;;) {
        int n = usb_serial_jtag_read_bytes(tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        feed_line(s_usb_line, &s_usb_line_len, tmp, (size_t)n, true);
    }
}

static void tcp_rx(void) {
    if (s_fd < 0) return;
    char tmp[256];
    for (;;) {
        int n = recv(s_fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            feed_line(s_tcp_line, &s_tcp_line_len, tmp, (size_t)n, false);
        } else if (n == 0) {
            tcp_close();
            break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            ESP_LOGW(TAG, "TCP 接收失败: errno=%d", errno);
            tcp_close();
            break;
        }
    }
}

static void net_event_handler(void *arg, esp_event_base_t base, int32_t id,
                              void *data) {
    (void)arg;
    net_evt_t ev = {0};
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ev.type = NET_EVT_START;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ev.type = NET_EVT_DISCONNECTED;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got = (ip_event_got_ip_t *)data;
        ev.type = NET_EVT_GOT_IP;
        ev.ip = got->ip_info.ip.addr;
    } else {
        return;
    }
    if (s_net_q) xQueueSend(s_net_q, &ev, 0);
}

static void net_events(void) {
    if (!s_net_q) return;
    net_evt_t ev;
    while (xQueueReceive(s_net_q, &ev, 0) == pdTRUE) {
        if (ev.type == NET_EVT_START) {
            if (s_wifi_started && kfb24_config_is_wifi(&s_cfg) && !s_wifi_connected) {
                esp_wifi_connect();
            }
        } else if (ev.type == NET_EVT_DISCONNECTED) {
            if (s_wifi_connected) ESP_LOGW(TAG, "Wi-Fi 断开");
            s_wifi_connected = false;
            s_ip[0] = '\0';
            tcp_close();
            s_wifi_retry_ms = now_ms() + WIFI_RETRY_MS;
        } else if (ev.type == NET_EVT_GOT_IP) {
            esp_ip4_addr_t addr = {.addr = ev.ip};
            esp_ip4addr_ntoa(&addr, s_ip, sizeof(s_ip));
            s_wifi_connected = true;
            s_wifi_fail_logs = 0;
            s_tcp_retry_ms = 0;
            s_tcp_fail_logged = false;
            tcp_close();
            ESP_LOGI(TAG, "Wi-Fi 已连接: %s", s_ip);
        }
    }
}

static void publish_status(void) {
    if (!s_status_lock) return;

    uint32_t now = now_ms();
    bool fresh = s_last_hello_ms != 0 && (now - s_last_hello_ms) < BRIDGE_TIMEOUT_MS;
    bool reachable = s_bridge_usb ? s_usb_connected : (s_fd >= 0);
    s_bridge_online = fresh && reachable;

    kfb24_link_status_t st;
    memset(&st, 0, sizeof(st));
    st.usb_connected = s_usb_connected;
    st.wifi_configured = kfb24_config_is_wifi(&s_cfg);
    st.wifi_connected = s_wifi_connected;
    st.tcp_connected = s_fd >= 0;
    st.bridge_online = s_bridge_online;
    st.bridge_usb = s_bridge_usb;
    if (s_bridge_online) {
        snprintf(st.transport, sizeof(st.transport), "%s",
                 s_bridge_usb ? "USB" : "WiFi");
    } else if (s_usb_connected) {
        snprintf(st.transport, sizeof(st.transport), "USB");
    } else if (s_wifi_connected) {
        snprintf(st.transport, sizeof(st.transport), "WiFi");
    } else {
        snprintf(st.transport, sizeof(st.transport), "-");
    }
    snprintf(st.ip, sizeof(st.ip), "%s", s_ip);
    snprintf(st.ssid, sizeof(st.ssid), "%s", s_cfg.ssid);

    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    uint32_t ack_seq = s_status.ack_seq;
    uint32_t ack_id = s_status.ack_id;
    bool ack_ok = s_status.ack_ok;
    st.ack_seq = ack_seq;
    st.ack_id = ack_id;
    st.ack_ok = ack_ok;
    s_status = st;
    xSemaphoreGive(s_status_lock);
}

static void link_task(void *arg) {
    (void)arg;
    if (kfb24_config_is_wifi(&s_cfg)) {
        esp_err_t err = wifi_apply(&s_cfg);
        if (err != ESP_OK) ESP_LOGE(TAG, "初始 Wi-Fi 启动失败");
    }

    for (;;) {
        uint32_t now = now_ms();

        usb_rx();
        tcp_rx();
        net_events();

        if (now - s_usb_poll_ms >= USB_POLL_MS) {
            s_usb_poll_ms = now;
            bool connected = s_usb_driver && usb_serial_jtag_is_connected();
            if (connected != s_usb_connected) {
                s_usb_connected = connected;
                ESP_LOGI(TAG, "%s", connected ? "USB 主机已连接" : "USB 主机已断开");
                if (connected) {
                    send_hello();
                    send_state(true);
                }
            }
        }

        if (s_wifi_started && !s_wifi_connected &&
            (int32_t)(now - s_wifi_retry_ms) >= 0) {
            s_wifi_retry_ms = now + WIFI_RETRY_MS;
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK && s_wifi_fail_logs < 3) {
                s_wifi_fail_logs++;
                ESP_LOGW(TAG, "Wi-Fi 重连失败: %s", esp_err_to_name(err));
            }
        }

        tcp_try_connect();

        cmd_item_t item;
        if (xQueueReceive(s_cmd_q, &item, 0) == pdTRUE) {
            char line[64];
            size_t len = kfb24_proto_fmt_cmd(line, sizeof(line), item.name, item.id);
            if (!len || !send_line_to_pc(line, len)) {
                ESP_LOGW(TAG, "命令发送失败: %s", item.name);
            } else {
                ESP_LOGI(TAG, "发送命令: %s #%lu", item.name,
                         (unsigned long)item.id);
            }
        }

        publish_status();
        send_state(false);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool kfb24_link_send_cmd(const char *name, uint32_t id) {
    if (!name || !s_cmd_q) return false;
    kfb24_link_status_t st;
    kfb24_link_get_status(&st);
    if (!st.bridge_online) return false;
    if (!st.usb_connected && !st.tcp_connected) return false;

    cmd_item_t item = {0};
    snprintf(item.name, sizeof(item.name), "%s", name);
    item.id = id;
    return xQueueSend(s_cmd_q, &item, 0) == pdTRUE;
}

void kfb24_link_start(const kfb24_cfg_t *cfg) {
    if (!cfg) return;
    s_cfg = *cfg;

    s_status_lock = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(8, sizeof(cmd_item_t));
    s_net_q = xQueueCreate(8, sizeof(net_evt_t));
    if (!s_status_lock || !s_cmd_q || !s_net_q) {
        ESP_LOGE(TAG, "链路队列/锁创建失败");
        return;
    }

    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        usb_cfg.tx_buffer_size = 2048;
        usb_cfg.rx_buffer_size = 2048;
        esp_err_t err = usb_serial_jtag_driver_install(&usb_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "USB 驱动安装失败: %s", esp_err_to_name(err));
        }
    }
    if (usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_vfs_use_driver();
        s_usb_driver = true;
    }

    if (xTaskCreate(link_task, "kfb24_link", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "链路任务创建失败");
    }
}
