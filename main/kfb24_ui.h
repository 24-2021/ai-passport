// kfb24_ui.h —— kfb24 screen and button handling.
#pragma once

#include "bsp_button.h"

void kfb24_ui_start(void);
void kfb24_ui_handle_input(bsp_btn_t btn, bsp_btn_ev_t ev);
