#pragma once

#include "lvgl.h"

typedef struct {
    lv_obj_t* time_label;
    lv_obj_t* wifi_icon;
    lv_obj_t* settings_icon;
} ScreenHeaderRefs;

void screen_prepare_root(lv_obj_t* scr);
ScreenHeaderRefs screen_header_create(lv_obj_t* parent, lv_event_cb_t settings_cb);
