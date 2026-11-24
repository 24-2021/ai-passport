#include "kfb24_ui.h"

#include "bsp_display.h"
#include "esp_log.h"
#include "kfb24_audio.h"
#include "kfb24_fonts.h"
#include "kfb24_link.h"
#include "kfb24_power.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define COL_BG 0x0B0F14
#define COL_SURFACE_HI 0x17252D
#define COL_BORDER 0x1E2A33
#define COL_TEXT 0xE8F0F4
#define COL_TEXT_DIM 0xAEBCC6
#define COL_MUTED 0x7C8B96
#define COL_ACCENT 0x5EEAD4
#define COL_WARNING 0xF2B84B
#define COL_ERROR 0xF26D6D

#define SCREEN_W 240
#define SCREEN_H 320

#define KFB24_ACTION_COUNT 8

static const char *TAG = "kfb24_ui";
static const char *CMDS[KFB24_ACTION_COUNT] = {
    "voice", "ime", "wechat", "orca",
    "terminal", "trae", "chrome", "edge",
};
static const char *NAMES[KFB24_ACTION_COUNT] = {
    "语音输入", "微信输入法", "微信", "Orca",
    "终端", "Trae", "Chrome", "Edge",
};
static const char *ICONS[KFB24_ACTION_COUNT] = {
    LV_SYMBOL_AUDIO, LV_SYMBOL_KEYBOARD, LV_SYMBOL_ENVELOPE,
    "O", ">_", "T", "C", "E",
};

static bool s_started;
static lv_obj_t *s_splash;
static lv_obj_t *s_home;
static lv_obj_t *s_dot;
static lv_obj_t *s_transport;
static lv_obj_t *s_batt_icon;
static lv_obj_t *s_batt_pct;
static lv_obj_t *s_hint;
static lv_obj_t *s_rows[KFB24_ACTION_COUNT];
static lv_obj_t *s_bars[KFB24_ACTION_COUNT];
static lv_obj_t *s_icons[KFB24_ACTION_COUNT];
static lv_obj_t *s_names[KFB24_ACTION_COUNT];
static lv_obj_t *s_about;
static lv_obj_t *s_about_values[6];
static int s_selected;
static uint32_t s_cmd_id;
static uint32_t s_pending_id;
static int s_pending_idx;
static uint32_t s_pending_ms;
static uint32_t s_seen_ack;
static uint32_t s_hint_set;
static bool s_voice_on;

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color,
                            int x, int y) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

static lv_obj_t *make_row(lv_obj_t *parent, int y,
                          const char *icon, const char *name, int index) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(row, 4, y);
    lv_obj_set_size(row, 232, 30);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_SURFACE_HI), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_outline_width(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *bar = lv_obj_create(row);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(bar, 0, 7);
    lv_obj_set_size(bar, 3, 16);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    lv_obj_t *icon_label = make_label(row, icon, &kfb24_font_16, COL_MUTED, 12, 6);
    lv_obj_set_width(icon_label, 20);
    lv_obj_t *name_label = make_label(row, name, &kfb24_font_14,
                                      COL_TEXT_DIM, 36, 7);
    lv_obj_set_width(name_label, 188);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_CLIP);

    s_rows[index] = row;
    s_bars[index] = bar;
    s_icons[index] = icon_label;
    s_names[index] = name_label;
    return row;
}
static void set_hint(const char *text, uint32_t color) {
    if (!s_hint) return;
    lv_label_set_text(s_hint, text);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(color), 0);
    s_hint_set = lv_tick_get();
}

static void default_hint(const kfb24_link_status_t *st) {
    if (!s_hint) return;
    const char *text;
    uint32_t color;
    if (st->bridge_online) {
        text = st->bridge_usb ? "已连接 · USB"
               : st->bridge_ble ? "已连接 · BLE"
                                : "已连接 · Wi-Fi";
        color = COL_ACCENT;
    } else if (st->ble_connected) {
        text = "BLE 已连接 · 等待电脑端";
        color = COL_WARNING;
    } else if (st->usb_connected) {
        text = "USB 已连接 · 等待电脑端";
        color = COL_WARNING;
    } else if (st->wifi_configured && st->wifi_connected) {
        text = "Wi-Fi 已连接 · 等待电脑端";
        color = COL_WARNING;
    } else if (st->wifi_configured) {
        text = "Wi-Fi 未连接";
        color = COL_MUTED;
    } else {
        text = "未连接电脑 · 请运行 PC 桥接";
        color = COL_MUTED;
    }
    lv_label_set_text(s_hint, text);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(color), 0);
    s_hint_set = 0;
}

static void refresh_selection(void) {
    for (int i = 0; i < KFB24_ACTION_COUNT; i++) {
        if (!s_rows[i]) continue;
        bool selected = i == s_selected;
        lv_obj_set_style_bg_opa(s_rows[i],
                                selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(s_bars[i],
                                  lv_color_hex(selected ? COL_ACCENT : COL_BORDER), 0);
        lv_obj_set_style_text_color(
            s_icons[i], lv_color_hex(selected ? COL_ACCENT : COL_MUTED), 0);
        lv_obj_set_style_text_color(
            s_names[i], lv_color_hex(selected ? COL_TEXT : COL_TEXT_DIM), 0);
    }
}

static uint32_t battery_color(int soc) {
    if (soc >= 0 && soc < 15) return COL_ERROR;
    if (soc >= 0 && soc < 30) return COL_WARNING;
    return COL_TEXT;
}

static const char *battery_symbol(int soc) {
    if (soc < 0) return LV_SYMBOL_BATTERY_EMPTY;
    if (soc >= 80) return LV_SYMBOL_BATTERY_FULL;
    if (soc >= 55) return LV_SYMBOL_BATTERY_3;
    if (soc >= 30) return LV_SYMBOL_BATTERY_2;
    if (soc >= 10) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void update_about(const kfb24_link_status_t *st, int soc, int mv) {
    if (!s_about) return;
    char buf[64];

    const char *transport = st->transport;
    const char *state = st->bridge_online ? "在线" : "离线";
    snprintf(buf, sizeof(buf), "%s · %s", transport, state);
    lv_label_set_text(s_about_values[0], buf);

    if (st->bridge_ble) {
        lv_label_set_text(s_about_values[1], "已连接 · 桥接");
    } else if (st->ble_connected) {
        lv_label_set_text(s_about_values[1], "已连接");
    } else {
        lv_label_set_text(s_about_values[1], "未连接");
    }

    if (!st->wifi_configured) {
        lv_label_set_text(s_about_values[2], "未配置");
    } else if (st->ssid[0]) {
        lv_label_set_text(s_about_values[2], st->ssid);
    } else {
        lv_label_set_text(s_about_values[2], "--");
    }

    if (st->wifi_connected && st->ip[0]) {
        lv_label_set_text(s_about_values[3], st->ip);
    } else {
        lv_label_set_text(s_about_values[3], "--");
    }

    if (soc >= 0) {
        snprintf(buf, sizeof(buf), "%d%% · %d mV", soc, mv);
    } else {
        snprintf(buf, sizeof(buf), "%s", "--");
    }
    lv_label_set_text(s_about_values[4], buf);
    lv_label_set_text(s_about_values[5], "kfb24 1.0.0");
}

static void update_status(void) {
    kfb24_link_status_t st;
    kfb24_link_get_status(&st);
    int soc = -1;
    int mv = -1;
    kfb24_power_get(&soc, &mv);

    if (s_dot) {
        uint32_t color = st.bridge_online ? COL_ACCENT
                         : (st.usb_connected || st.wifi_connected ||
                            st.ble_connected) ? COL_WARNING
                                             : COL_MUTED;
        lv_obj_set_style_bg_color(s_dot, lv_color_hex(color), 0);
    }
    if (s_transport) {
        lv_label_set_text(s_transport, st.transport[0] ? st.transport : "--");
        lv_obj_set_style_text_color(
            s_transport,
            lv_color_hex(st.bridge_online ? COL_ACCENT
                          : (st.usb_connected || st.wifi_connected ||
                             st.ble_connected) ? COL_WARNING
                                               : COL_MUTED),
            0);
    }
    if (s_batt_icon && s_batt_pct) {
        lv_label_set_text(s_batt_icon, battery_symbol(soc));
        if (soc >= 0) {
            lv_label_set_text_fmt(s_batt_pct, "%d%%", soc);
        } else {
            lv_label_set_text(s_batt_pct, "--");
        }
        lv_obj_set_style_text_color(s_batt_icon, lv_color_hex(battery_color(soc)), 0);
        lv_obj_set_style_text_color(s_batt_pct, lv_color_hex(battery_color(soc)), 0);
    }

    if (s_hint && s_hint_set && lv_tick_elaps(s_hint_set) > 1800) {
        default_hint(&st);
    } else if (s_hint && !s_hint_set) {
        default_hint(&st);
    }

    if (s_pending_id && lv_tick_elaps(s_pending_ms) > 3000) {
        s_pending_id = 0;
        set_hint("电脑端无应答", COL_WARNING);
    }
    update_about(&st, soc, mv);
}

static void execute_action(int index) {
    if (!s_home || index < 0 || index >= KFB24_ACTION_COUNT) return;
    kfb24_link_status_t st;
    kfb24_link_get_status(&st);
    if (!st.bridge_online) {
        set_hint("电脑桥接未运行", COL_WARNING);
        return;
    }

    bool voice = index == 0;
    if (!voice && s_voice_on) {
        kfb24_audio_stop();
        s_voice_on = false;
    }

    s_cmd_id++;
    if (s_cmd_id == 0) s_cmd_id = 1;
    if (!kfb24_link_send_cmd(CMDS[index], s_cmd_id)) {
        set_hint("发送失败 · 请重试", COL_ERROR);
        return;
    }
    s_pending_id = s_cmd_id;
    s_pending_idx = index;
    s_pending_ms = lv_tick_get();

    if (voice) {
        if (s_voice_on) {
            kfb24_audio_stop();
            s_voice_on = false;
            set_hint("录音已停止", COL_ACCENT);
            return;
        }
        if (st.bridge_usb && kfb24_audio_start()) {
            s_voice_on = true;
            set_hint("录音中 · 再按停止", COL_ACCENT);
        } else {
            set_hint("使用电脑麦克风 · 再按停止", COL_ACCENT);
        }
        return;
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "已发送 · %s", NAMES[index]);
    set_hint(buf, COL_ACCENT);
}

static void card_clicked(lv_event_t *event) {
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= KFB24_ACTION_COUNT) return;
    s_selected = index;
    refresh_selection();
    execute_action(index);
}

static void about_close(void) {
    if (!s_about) return;
    lv_obj_delete(s_about);
    s_about = NULL;
    for (int i = 0; i < 6; i++) s_about_values[i] = NULL;
}

static lv_obj_t *about_row(lv_obj_t *parent, const char *name, int y) {
    make_label(parent, name, &kfb24_font_14, COL_MUTED, 16, y);
    lv_obj_t *value = make_label(parent, "--", &kfb24_font_14, COL_TEXT, 76, y);
    lv_obj_set_width(value, 148);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    return value;
}

static void about_open(void) {
    if (s_about || !s_home) return;

    lv_obj_t *overlay = lv_obj_create(s_home);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);

    make_label(overlay, "关于", &kfb24_font_22, COL_TEXT, 16, 14);
    lv_obj_t *version = make_label(overlay, "kfb24 1.0.0",
                                   &lv_font_montserrat_14, COL_MUTED, 16, 46);
    lv_obj_set_width(version, 208);
    lv_obj_set_style_text_align(version, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *divider = lv_obj_create(overlay);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(divider, 16, 68);
    lv_obj_set_size(divider, 208, 1);
    lv_obj_set_style_radius(divider, 0, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);

    s_about_values[0] = about_row(overlay, "连接", 84);
    s_about_values[1] = about_row(overlay, "BLE", 114);
    s_about_values[2] = about_row(overlay, "网络", 144);
    s_about_values[3] = about_row(overlay, "地址", 174);
    s_about_values[4] = about_row(overlay, "电量", 204);
    s_about_values[5] = about_row(overlay, "固件", 234);

    lv_obj_t *rule = lv_obj_create(overlay);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(rule, 16, 276);
    lv_obj_set_size(rule, 208, 1);
    lv_obj_set_style_radius(rule, 0, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(rule, 0, 0);

    lv_obj_t *close = make_label(overlay, "按 OK 关闭", &kfb24_font_14,
                                 COL_MUTED, 16, 288);
    lv_obj_set_width(close, 208);
    lv_obj_set_style_text_align(close, LV_TEXT_ALIGN_RIGHT, 0);

    s_about = overlay;
    update_status();
}

static void status_tick(lv_timer_t *timer) {
    (void)timer;
    kfb24_link_status_t st;
    kfb24_link_get_status(&st);
    if (s_voice_on && !kfb24_audio_is_active()) {
        s_voice_on = false;
        set_hint("录音已停止", COL_WARNING);
    }
    if (st.ack_seq != s_seen_ack) {
        s_seen_ack = st.ack_seq;
        if (s_pending_id && st.ack_id == s_pending_id) {
            char buf[48];
            if (s_pending_idx == 0 && s_voice_on && !st.ack_ok) {
                kfb24_audio_stop();
                s_voice_on = false;
            }
            if (s_pending_idx == 0 && s_voice_on) {
                snprintf(buf, sizeof(buf), "录音中 · 再按停止");
            } else {
                snprintf(buf, sizeof(buf), "%s · %s", st.ack_ok ? "已执行" : "执行失败",
                         NAMES[s_pending_idx]);
            }
            set_hint(buf, st.ack_ok ? COL_ACCENT : COL_ERROR);
            s_pending_id = 0;
        }
    }
    update_status();
}

static void splash_done(lv_timer_t *timer) {
    lv_timer_delete(timer);
    if (!s_splash) return;

    lv_obj_t *home = lv_obj_create(NULL);
    s_home = home;
    lv_obj_remove_flag(home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(home, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(home, 0, 0);

    lv_obj_t *wordmark = make_label(home, "kfb24", &lv_font_montserrat_16,
                                    COL_ACCENT, 14, 5);
    lv_obj_set_style_text_letter_space(wordmark, 1, 0);

    s_dot = lv_obj_create(home);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_dot, 62, 13);
    lv_obj_set_size(s_dot, 6, 6);
    lv_obj_set_style_radius(s_dot, 3, 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);

    s_transport = make_label(home, "--", &lv_font_montserrat_14, COL_MUTED, 72, 5);
    lv_obj_set_width(s_transport, 60);

    s_batt_icon = make_label(home, LV_SYMBOL_BATTERY_EMPTY,
                             &lv_font_montserrat_14, COL_TEXT, 152, 5);
    s_batt_pct = make_label(home, "--", &lv_font_montserrat_14, COL_TEXT, 170, 5);
    lv_obj_set_width(s_batt_pct, 54);
    lv_obj_set_style_text_align(s_batt_pct, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *rule = lv_obj_create(home);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(rule, 12, 30);
    lv_obj_set_size(rule, 216, 1);
    lv_obj_set_style_radius(rule, 0, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(rule, 0, 0);

    for (int i = 0; i < KFB24_ACTION_COUNT; i++) {
        lv_obj_t *row = make_row(home, 36 + i * 30,
                                 ICONS[i], NAMES[i], i);
        lv_obj_add_event_cb(row, card_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }

    s_hint = make_label(home, "", &kfb24_font_14, COL_MUTED, 12, 277);
    lv_obj_set_width(s_hint, 216);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_DOT);

    lv_obj_t *foot_rule = lv_obj_create(home);
    lv_obj_remove_flag(foot_rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(foot_rule, 12, 298);
    lv_obj_set_size(foot_rule, 216, 1);
    lv_obj_set_style_radius(foot_rule, 0, 0);
    lv_obj_set_style_border_width(foot_rule, 0, 0);
    lv_obj_set_style_bg_color(foot_rule, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_bg_opa(foot_rule, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(foot_rule, 0, 0);

    make_label(home, "OK 执行 · 长按 关于", &kfb24_font_14, COL_MUTED, 12, 301);
    lv_obj_t *by = make_label(home, "by 24", &lv_font_montserrat_14,
                              COL_ACCENT, 188, 301);
    lv_obj_set_width(by, 40);
    lv_obj_set_style_text_align(by, LV_TEXT_ALIGN_RIGHT, 0);

    s_selected = 0;
    refresh_selection();
    update_status();

    lv_screen_load(home);
    lv_obj_delete(s_splash);
    s_splash = NULL;
    lv_timer_create(status_tick, 500, NULL);
}

static lv_obj_t *make_splash(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *name = make_label(screen, "kfb24", &lv_font_montserrat_28,
                                COL_TEXT, 0, 122);
    lv_obj_set_width(name, SCREEN_W);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(name, 2, 0);

    lv_obj_t *rule = lv_obj_create(screen);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(rule, 108, 162);
    lv_obj_set_size(rule, 24, 2);
    lv_obj_set_style_radius(rule, 1, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(rule, 0, 0);

    lv_obj_t *tag = make_label(screen, "PC COMMAND CONSOLE",
                               &lv_font_montserrat_14, COL_MUTED, 0, 178);
    lv_obj_set_width(tag, SCREEN_W);
    lv_obj_set_style_text_align(tag, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(tag, 2, 0);

    lv_obj_t *foot = make_label(screen, "ESP32-C3 · USB / Wi-Fi / BLE",
                                &lv_font_montserrat_14, COL_MUTED, 0, 286);
    lv_obj_set_width(foot, SCREEN_W);
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, 0);
    return screen;
}

void kfb24_ui_start(void) {
    if (s_started) return;
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL 锁超时，UI 未启动");
        return;
    }
    s_started = true;
    s_splash = make_splash();
    lv_screen_load(s_splash);
    lv_timer_create(splash_done, 1000, NULL);
    bsp_lvgl_unlock();
}

void kfb24_ui_handle_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (!bsp_lvgl_lock(300)) return;
    if (!s_home) {
        bsp_lvgl_unlock();
        return;
    }

    if (s_about) {
        if (btn == BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
            about_close();
        }
        bsp_lvgl_unlock();
        return;
    }

    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        about_open();
        bsp_lvgl_unlock();
        return;
    }
    if (ev != BSP_BTN_CLICK) {
        bsp_lvgl_unlock();
        return;
    }

    if (btn == BSP_BTN_UP) {
        s_selected = (s_selected + KFB24_ACTION_COUNT - 1) % KFB24_ACTION_COUNT;
        refresh_selection();
    } else if (btn == BSP_BTN_DOWN) {
        s_selected = (s_selected + 1) % KFB24_ACTION_COUNT;
        refresh_selection();
    } else if (btn == BSP_BTN_OK) {
        execute_action(s_selected);
    }
    bsp_lvgl_unlock();
}
