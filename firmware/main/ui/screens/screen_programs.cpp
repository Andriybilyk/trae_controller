#include "screens.h"
#include "screen_header.h"
#include "theme.h"
#include "kiln_control/thermal_control.h"

static lv_obj_t* programs_list = nullptr;

static void back_event_cb(lv_event_t* e) {
    /*
    (void)e;
    screens_request_show(SCREEN_MAIN);
    */
}

static void select_program_cb(lv_event_t* e) {
    /*
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    (void)slot;
    screens_request_show(SCREEN_MAIN);
    */
}

void screen_programs_create() {
    lv_obj_t* scr = lv_obj_create(NULL);
    screen_prepare_root(scr);
    screen_header_create(scr, nullptr);

    lv_obj_t* page_title = lv_label_create(scr);
    lv_label_set_text(page_title, "Програми");
    lv_obj_set_style_text_font(page_title, FONT_XXL, 0);
    lv_obj_set_style_text_color(page_title, COL_TEXT, 0);
    lv_obj_set_pos(page_title, 20, 72);

    programs_list = lv_list_create(scr);
    lv_obj_set_size(programs_list, SCREEN_W - 40, SCREEN_H - 120 - 20);
    lv_obj_set_pos(programs_list, 20, 120);
    lv_obj_set_style_bg_color(programs_list, COL_BG, 0);
    lv_obj_set_style_border_width(programs_list, 0, 0);
    lv_obj_set_style_pad_all(programs_list, 0, 0);
    lv_obj_set_style_pad_row(programs_list, 8, 0);

    const char* programs[] = {
        "Програма 1: Поливний випал 900°C",
        "Програма 2: Поливний випал 1100°C",
        "Програма 3: Глазурування",
        "Програма 4: Загартування",
        "Програма 5: Спеціальна"
    };

    for (int i = 0; i < 5; i++) {
        lv_obj_t* btn = lv_list_add_btn(programs_list, nullptr, programs[i]);
        lv_obj_set_size(btn, SCREEN_W - 64, 60);
        lv_obj_set_style_bg_color(btn, COL_BG_CARD, 0);
        lv_obj_set_style_radius(btn, RADIUS_CARD, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, COL_BORDER, 0);
        lv_obj_set_style_text_font(btn, FONT_TINY, 0);
        lv_obj_set_style_text_color(btn, COL_TEXT, 0);
        lv_obj_set_style_pad_left(btn, 16, 0);
        lv_obj_add_event_cb(btn, select_program_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
}

void screen_programs_update() {
}
