#include "screen_header.h"

#include "theme.h"
#include "picopixel/images.h"

#include <ctime>

static void set_header_time(lv_obj_t* label) {
    if (label == nullptr) {
        return;
    }

    time_t ts = time(nullptr);
    struct tm tmv {};
    localtime_r(&ts, &tmv);

    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M %d.%m.%Y", &tmv);
    lv_label_set_text(label, buf);
}

void screen_prepare_root(lv_obj_t* scr) {
    if (scr == nullptr) {
        return;
    }

    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, 146, 0);
    lv_obj_set_style_radius(scr, 0, 0);
}

ScreenHeaderRefs screen_header_create(lv_obj_t* parent, lv_event_cb_t settings_cb) {
    ScreenHeaderRefs refs {};

    lv_obj_t* logo_panel = lv_obj_create(parent);
    lv_obj_set_size(logo_panel, 32, 32);
    lv_obj_set_pos(logo_panel, 25, 13);
    lv_obj_set_style_bg_color(logo_panel, COL_ACCENT, 0);
    lv_obj_set_style_radius(logo_panel, 5, 0);
    lv_obj_set_style_border_width(logo_panel, 0, 0);
    lv_obj_set_style_pad_all(logo_panel, 0, 0);

    lv_obj_t* title = lv_label_create(parent);
    lv_label_set_text(title, "KilnPro");
    lv_obj_set_style_text_font(title, FONT_XXL, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_set_style_text_decor(title, LV_TEXT_DECOR_NONE, 0);
    lv_obj_set_pos(title, 64, 14);

    refs.time_label = lv_label_create(parent);
    lv_obj_set_style_text_font(refs.time_label, FONT_TINY, 0);
    lv_obj_set_style_text_color(refs.time_label, COL_TEXT, 0);
    lv_obj_set_pos(refs.time_label, 418, 25);
    set_header_time(refs.time_label);

    refs.wifi_icon = lv_img_create(parent);
    lv_obj_set_pos(refs.wifi_icon, 596, 16);
    lv_obj_set_size(refs.wifi_icon, 32, 32);
    lv_obj_set_style_img_recolor_opa(refs.wifi_icon, 255, 0);
    lv_obj_set_style_img_recolor(refs.wifi_icon, COL_ACCENT, 0);
    lv_img_set_src(refs.wifi_icon, &pngwing_com_1_png);
    lv_obj_add_flag(refs.wifi_icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lang_panel = lv_obj_create(parent);
    lv_obj_set_size(lang_panel, 45, 32);
    lv_obj_set_pos(lang_panel, 664, 16);
    lv_obj_set_style_pad_all(lang_panel, 0, 0);
    lv_obj_set_style_bg_color(lang_panel, lv_color_hex(0xFF3D3D3D), 0);
    lv_obj_set_style_radius(lang_panel, 5, 0);
    lv_obj_set_style_border_width(lang_panel, 0, 0);

    lv_obj_t* lang_label = lv_label_create(lang_panel);
    lv_label_set_text(lang_label, "UA");
    lv_obj_set_style_text_font(lang_label, FONT_TINY, 0);
    lv_obj_set_style_text_color(lang_label, COL_TEXT, 0);
    lv_obj_center(lang_label);

    refs.settings_icon = lv_img_create(parent);
    lv_obj_set_pos(refs.settings_icon, 745, 16);
    lv_obj_set_size(refs.settings_icon, 32, 32);
    lv_obj_set_style_img_recolor_opa(refs.settings_icon, 255, 0);
    lv_obj_set_style_img_recolor(refs.settings_icon, COL_TEXT_DIM, 0);
    lv_img_set_src(refs.settings_icon, &pngwing_com_png);
    lv_obj_add_flag(refs.settings_icon, LV_OBJ_FLAG_CLICKABLE);
    if (settings_cb != nullptr) {
        lv_obj_add_event_cb(refs.settings_icon, settings_cb, LV_EVENT_CLICKED, nullptr);
    }

    return refs;
}
