#pragma once

#include <lvgl.h>

void display_driver_init(void);

void display_driver_lvgl_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p);
void display_driver_lvgl_touch_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data);

