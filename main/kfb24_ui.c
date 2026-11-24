#include "kfb24_ui.h"

#include "bsp_display.h"
#include "esp_log.h"
#include "kfb24_fonts.h"
#include "kfb24_link.h"
#include "kfb24_power.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define COL_BG 0x0B0F14
#define COL_SURFACE 0x121A21
#define COL_SURFACE_HI 0x17252D
#define COL_BORDER 0x1E2A33
#define COL_TEXT 0xE8F0F4
#define COL_MUTED 0x7C8B96
#define COL_ACCENT 0x5EEAD4
#define COL_ACCENT_DARK 0x07221F
#define COL_WARNING 0xF2B84B
#define COL_ERROR 0xF26D6D

#define SCREEN_W 240
#define SCREEN_H 320

static const char *TAG = "kfb24_ui";
static const char *CMDS[5] = {"voice", "ime", "wechat", "orca", "terminal"};
static const char *NAMES[5] = {"语音识别", "微信输入法", "打开微信", "Orca", "终端"};
static const char *ICONS[5] = {LV_SYMBOL_AUDIO, LV_SYMBOL_KEYBOARD,
                               LV_SYMBOL_ENVELOPE, "O", ">_"};

static bool s_started;
static lv_obj_t *s_splash;
static lv_obj_t *s_home;
static lv_obj_t *s_dot;
static lv_obj_t *s_transport;
static lv_obj_t *s_batt_icon;
static lv_obj_t *s_batt_pct;
static lv_obj_t *s_hint;
static lv_obj_t *s_cards[5];
static lv_obj_t *s_icons[5];
static lv_obj_t *s_names[5];
static lv_obj_t *s_about;
static lv_obj_t *s_about_values[5];
static int s_selected;
static uint32_t s_cmd_id;
static uint32_t s_pending_id;
static int s_pending_idx;
static uint32_t s_pending_ms;
static uint32_t s_seen_ack;
static uint32_t s_hint_set;

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

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h,
                           const char *icon, const char *name, int index,
                           bool hero) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_radius(card, hero ? 18 : 16, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(hero ? COL_ACCENT : COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(card, hero ? 0 : 1, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_outline_width(card, 0, 0);
    lv_obj_set_style_outline_color(card, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_outline_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(card, hero ? 10 : 0, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_shadow_opa(card, 60, 0);
    lv_obj_set_style_pad_all(card, 0, 0);

    lv_obj_t *icon_label = make_label(card, icon,
                                      hero ? &lv_font_montserrat_28
                                           : &lv_font_montserrat_20,
                                      hero ? COL_ACCENT_DARK : COL_MUTED,
                                      hero ? 14 : 10, hero ? 15 : 4);
    lv_obj_t *name_label = make_label(card, name,
                                      hero ? &kfb24_font_22 : &kfb24_font_16,
                                      hero ? COL_ACCENT_DARK : COL_TEXT,
                                      hero ? 52 : 8, hero ? 6 : 30);
    lv_obj_set_width(name_label, hero ? 152 : 88);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_CLIP);

    if (hero) {
        make_label(card, "电脑麦克风 · Win+H", &kfb24_font_14,
                   COL_ACCENT_DARK, 52, 34);
    }

    s_cards[index] = card;
    s_icons[index] = icon_label;
    s_names[index] = name_label;
    return card;
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
        text = st->bridge_usb ? "已连接 · USB" : "已连接 · Wi-Fi";
        color = COL_ACCENT;
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
    for (int i = 0; i < 5; i++) {
        if (!s_cards[i]) continue;
        bool selected = i == s_selected;
        if (i == 0) {
            lv_obj_set_style_outline_width(s_cards[i], selected ? 2 : 0, 0);
            lv_obj_set_style_bg_color(s_cards[i], lv_color_hex(COL_ACCENT), 0);
        } else {
            lv_obj_set_style_border_width(s_cards[i], selected ? 2 : 1, 0);
            lv_obj_set_style_border_color(
                s_cards[i], lv_color_hex(selected ? COL_ACCENT : COL_BORDER), 0);
            lv_obj_set_style_bg_color(
                s_cards[i], lv_color_hex(selected ? COL_SURFACE_HI : COL_SURFACE), 0);
            lv_obj_set_style_text_color(
                s_icons[i], lv_color_hex(selected ? COL_ACCENT : COL_MUTED), 0);
            lv_obj_set_style_text_color(
                s_names[i], lv_color_hex(COL_TEXT), 0);
        }
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

    if (!st->wifi_configured) {
        lv_label_set_text(s_about_values[1], "未配置");
    } else if (st->ssid[0]) {
        lv_label_set_text(s_about_values[1], st->ssid);
    } else {
        lv_label_set_text(s_about_values[1], "--");
    }

    if (st->wifi_connected && st->ip[0]) {
        lv_label_set_text(s_about_values[2], st->ip);
    } else {
        lv_label_set_text(s_about_values[2], "--");
    }

    if (soc >= 0) {
        snprintf(buf, sizeof(buf), "%d%% · %d mV", soc, mv);
    } else {
        snprintf(buf, sizeof(buf), "%s", "--");
    }
    lv_label_set_text(s_about_values[3], buf);
    lv_label_set_text(s_about_values[4], "kfb24 1.0.0");
}

static void update_status(void) {
    kfb24_link_status_t st;
    kfb24_link_get_status(&st);
    int soc = -1;
    int mv = -1;
    kfb24_power_get(&soc, &mv);

    if (s_dot) {
        uint32_t color = st.bridge_online ? COL_ACCENT
                         : (st.usb_connected || st.wifi_connected) ? COL_WARNING
                                                                   : COL_MUTED;
        lv_obj_set_style_bg_color(s_dot, lv_color_hex(color), 0);
    }
    if (s_transport) {
        lv_label_set_text(s_transport, st.transport[0] ? st.transport : "--");
        lv_obj_set_style_text_color(
            s_transport,
            lv_color_hex(st.bridge_online ? COL_ACCENT
                          : (st.usb_connected || st.wifi_connected) ? COL_WARNING
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
    if (!s_home || index < 0 || index >= 5) return;
    kfb24_link_status_t st;
    kfb24_link_get_status(&st);
    if (!st.bridge_online) {
        set_hint("电脑桥接未运行", COL_WARNING);
        return;
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

    char buf[48];
    snprintf(buf, sizeof(buf), "已发送 · %s", NAMES[index]);
    set_hint(buf, COL_ACCENT);
}

static void card_clicked(lv_event_t *event) {
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= 5) return;
    s_selected = index;
    refresh_selection();
    execute_action(index);
}

static void about_close(void) {
    if (!s_about) return;
    lv_obj_delete(s_about);
    s_about = NULL;
    for (int i = 0; i < 5; i++) s_about_values[i] = NULL;
}

static lv_obj_t *about_row(lv_obj_t *parent, const char *name, int y) {
    make_label(parent, name, &kfb24_font_14, COL_MUTED, 16, y);
    lv_obj_t *value = make_label(parent, "--", &kfb24_font_14, COL_TEXT, 72, y);
    lv_obj_set_width(value, 116);
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
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, 180, 0);

    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(card, 18, 44);
    lv_obj_set_size(card, 204, 232);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_pad_all(card, 0, 0);

    make_label(card, "关于 kfb24", &kfb24_font_22, COL_TEXT, 16, 12);
    s_about_values[0] = about_row(card, "连接", 52);
    s_about_values[1] = about_row(card, "网络", 76);
    s_about_values[2] = about_row(card, "地址", 100);
    s_about_values[3] = about_row(card, "电量", 124);
    s_about_values[4] = about_row(card, "固件", 148);

    lv_obj_t *divider = lv_obj_create(card);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(divider, 16, 176);
    lv_obj_set_size(divider, 172, 1);
    lv_obj_set_style_radius(divider, 0, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);

    lv_obj_t *by = make_label(card, "by 24", &lv_font_montserrat_14,
                              COL_ACCENT, 0, 184);
    lv_obj_set_width(by, 204);
    lv_obj_set_style_text_align(by, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *close = make_label(card, "按 OK 关闭", &kfb24_font_14,
                                 COL_MUTED, 0, 204);
    lv_obj_set_width(close, 204);
    lv_obj_set_style_text_align(close, LV_TEXT_ALIGN_CENTER, 0);

    s_about = overlay;
    update_status();
}

static void status_tick(lv_timer_t *timer) {
    (void)timer;
    kfb24_link_status_t st;
    kfb24_link_get_status(&st);
    if (st.ack_seq != s_seen_ack) {
        s_seen_ack = st.ack_seq;
        if (s_pending_id && st.ack_id == s_pending_id) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%s · %s", st.ack_ok ? "已执行" : "执行失败",
                     NAMES[s_pending_idx]);
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

    lv_obj_t *wordmark = make_label(home, "kfb24", &lv_font_montserrat_20,
                                    COL_ACCENT, 16, 8);
    lv_obj_set_style_text_letter_space(wordmark, 1, 0);

    s_dot = lv_obj_create(home);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_dot, 84, 20);
    lv_obj_set_size(s_dot, 8, 8);
    lv_obj_set_style_radius(s_dot, 4, 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);

    s_transport = make_label(home, "--", &lv_font_montserrat_14, COL_MUTED, 96, 12);
    lv_obj_set_width(s_transport, 48);

    lv_obj_t *battery = lv_obj_create(home);
    lv_obj_remove_flag(battery, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(battery, 154, 12);
    lv_obj_set_size(battery, 72, 22);
    lv_obj_set_style_radius(battery, 11, 0);
    lv_obj_set_style_bg_color(battery, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(battery, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery, 1, 0);
    lv_obj_set_style_border_color(battery, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_pad_all(battery, 0, 0);
    s_batt_icon = make_label(battery, LV_SYMBOL_BATTERY_EMPTY,
                             &lv_font_montserrat_14, COL_TEXT, 8, 3);
    s_batt_pct = make_label(battery, "--", &lv_font_montserrat_14, COL_TEXT, 26, 3);
    lv_obj_set_width(s_batt_pct, 38);
    lv_obj_set_style_text_align(s_batt_pct, LV_TEXT_ALIGN_RIGHT, 0);

    make_label(home, "快捷控制", &kfb24_font_22, COL_TEXT, 16, 40);
    s_hint = make_label(home, "PC COMMAND CONSOLE", &kfb24_font_14, COL_MUTED, 16, 70);
    lv_obj_set_width(s_hint, 208);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_DOT);

    lv_obj_t *hero = make_card(home, 12, 88, 216, 58, ICONS[0], NAMES[0], 0, true);
    lv_obj_add_event_cb(hero, card_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)0);

    lv_obj_t *ime = make_card(home, 12, 154, 104, 58, ICONS[1], NAMES[1], 1, false);
    lv_obj_add_event_cb(ime, card_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *wechat = make_card(home, 124, 154, 104, 58, ICONS[2], NAMES[2], 2, false);
    lv_obj_add_event_cb(wechat, card_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)2);
    lv_obj_t *orca = make_card(home, 12, 220, 104, 58, ICONS[3], NAMES[3], 3, false);
    lv_obj_add_event_cb(orca, card_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)3);
    lv_obj_t *terminal = make_card(home, 124, 220, 104, 58, ICONS[4], NAMES[4], 4, false);
    lv_obj_add_event_cb(terminal, card_clicked, LV_EVENT_CLICKED,
                        (void *)(intptr_t)4);

    lv_obj_t *rule = lv_obj_create(home);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(rule, 24, 284);
    lv_obj_set_size(rule, 192, 1);
    lv_obj_set_style_radius(rule, 0, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(rule, 0, 0);

    make_label(home, "UP/DOWN 选择 · OK 执行", &kfb24_font_14, COL_MUTED, 24, 292);
    lv_obj_t *by = make_label(home, "by 24", &lv_font_montserrat_14,
                              COL_ACCENT, 176, 292);
    lv_obj_set_width(by, 48);
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

    lv_obj_t *logo = lv_obj_create(screen);
    lv_obj_remove_flag(logo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(logo, 88, 52);
    lv_obj_set_size(logo, 64, 64);
    lv_obj_set_style_radius(logo, 20, 0);
    lv_obj_set_style_bg_color(logo, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(logo, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(logo, 0, 0);
    lv_obj_set_style_pad_all(logo, 0, 0);
    lv_obj_t *mark = make_label(logo, "24", &lv_font_montserrat_28,
                                COL_ACCENT_DARK, 0, 14);
    lv_obj_set_width(mark, 64);
    lv_obj_set_style_text_align(mark, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *name = make_label(screen, "kfb24", &lv_font_montserrat_28,
                                COL_TEXT, 0, 132);
    lv_obj_set_width(name, SCREEN_W);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(name, 2, 0);

    lv_obj_t *tag = make_label(screen, "PC COMMAND CONSOLE",
                               &lv_font_montserrat_14, COL_MUTED, 0, 172);
    lv_obj_set_width(tag, SCREEN_W);
    lv_obj_set_style_text_align(tag, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(tag, 2, 0);

    lv_obj_t *rule = lv_obj_create(screen);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(rule, 108, 198);
    lv_obj_set_size(rule, 24, 2);
    lv_obj_set_style_radius(rule, 1, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(rule, 0, 0);

    lv_obj_t *by = make_label(screen, "by 24", &lv_font_montserrat_14,
                              COL_ACCENT, 0, 210);
    lv_obj_set_width(by, SCREEN_W);
    lv_obj_set_style_text_align(by, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *foot = make_label(screen, "ESP32-C3 · USB / Wi-Fi",
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
        s_selected = (s_selected + 4) % 5;
        refresh_selection();
    } else if (btn == BSP_BTN_DOWN) {
        s_selected = (s_selected + 1) % 5;
        refresh_selection();
    } else if (btn == BSP_BTN_OK) {
        execute_action(s_selected);
    }
    bsp_lvgl_unlock();
}
