// kfb24_ui.h —— kfb24 screen and button handling.
#pragma once

#include "bsp_button.h"

void kfb24_ui_start(void);
void kfb24_ui_handle_input(bsp_btn_t btn, bsp_btn_ev_t ev);

// Shows a PC -> board dialogue bubble ("KFB24 TEXT" payload, UTF-8). Takes the
// LVGL lock internally and never blocks longer than a few milliseconds; the
// bubble hides itself after 15 s.
void kfb24_ui_set_text(const char *utf8);
