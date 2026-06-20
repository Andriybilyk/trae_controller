#include "screens.h"
#include "screen_header.h"
#include "theme.h"

static void back_event_cb(lv_event_t* e) {
    (void)e;
    // screens_request_show(SCREEN_ID_MAIN);
}

void screen_settings_create() {
    lv_obj_t* scr = lv_obj_create(NULL);
    screen_prepare_root(scr);
    screen_header_create(scr, back_event_cb);

    lv_obj_t* page_title = lv_label_create(scr);
    lv_label_set_text(page_title, "Налаштування");
    lv_obj_set_style_text_font(page_title, FONT_XXL, 0);
    lv_obj_set_style_text_color(page_title, COL_TEXT, 0);
    lv_obj_set_pos(page_title, 20, 72);

    lv_obj_t* content = lv_obj_create(scr);
    lv_obj_set_size(content, SCREEN_W - 40, SCREEN_H - 120 - 20);
    lv_obj_set_pos(content, 20, 120);
    lv_obj_set_style_bg_color(content, COL_BG_CARD, 0);
    lv_obj_set_style_radius(content, RADIUS_CARD, 0);
    lv_obj_set_style_border_width(content, 1, 0);
    lv_obj_set_style_border_color(content, COL_BORDER, 0);

    lv_obj_t* settings_title = lv_label_create(content);
    lv_label_set_text(settings_title, "Налаштування пічки");
    lv_obj_set_style_text_font(settings_title, FONT_NORMAL, 0);
    lv_obj_set_style_text_color(settings_title, COL_TEXT, 0);
    lv_obj_set_pos(settings_title, 16, 20);

    lv_obj_t* temp_unit_label = lv_label_create(content);
    lv_label_set_text(temp_unit_label, "Одиниці температури: °C");
    lv_obj_set_style_text_font(temp_unit_label, FONT_TINY, 0);
    lv_obj_set_style_text_color(temp_unit_label, COL_TEXT_DIM, 0);
    lv_obj_set_pos(temp_unit_label, 16, 70);

    lv_obj_t* max_temp_label = lv_label_create(content);
    lv_label_set_text(max_temp_label, "Максимальна температура: 1300°C");
    lv_obj_set_style_text_font(max_temp_label, FONT_TINY, 0);
    lv_obj_set_style_text_color(max_temp_label, COL_TEXT_DIM, 0);
    lv_obj_set_pos(max_temp_label, 16, 110);

    lv_obj_t* version_label = lv_label_create(content);
    lv_label_set_text(version_label, "Версія прошивки: v1.0.0");
    lv_obj_set_style_text_font(version_label, FONT_TINY, 0);
    lv_obj_set_style_text_color(version_label, COL_TEXT_DIM, 0);
    lv_obj_set_pos(version_label, 16, 150);
}
