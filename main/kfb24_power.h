// kfb24_power.h —— cached CW2017 readings for the LVGL task.
#pragma once

void kfb24_power_start(void);
void kfb24_power_get(int *soc, int *mv);
