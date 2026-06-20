#include "screens.h"
#include "screen_header.h"
#include "theme.h"
#include "kiln_control/thermal_control.h"
#include "esp_log.h"
#include <cstdio>

static const char* TAG = "UI_DBG";

void screen_program_edit_create(int slot) {
    (void)slot;

    lv_obj_t* scr = lv_obj_create(NULL);
    screen_prepare_root(scr);
    screen_header_create(scr, nullptr);

    lv_obj_t* page_title = lv_label_create(scr);
    lv_label_set_text(page_title, "Редагування програми");
    lv_obj_set_style_text_font(page_title, FONT_XXL, 0);
    lv_obj_set_style_text_color(page_title, COL_TEXT, 0);
    lv_obj_set_pos(page_title, 20, 72);

    static lv_style_t style_table_header;
    lv_style_init(&style_table_header);
    lv_style_set_bg_color(&style_table_header, lv_color_hex(0x282c34));
    lv_style_set_text_color(&style_table_header, lv_color_white());

    // Steps Table
    lv_obj_t* table = lv_table_create(scr);
    lv_obj_set_pos(table, 20, 120);
    lv_obj_set_size(table, SCREEN_W - 40, SCREEN_H - 120 - 30);
    lv_obj_set_style_bg_color(table, COL_BG_CARD, 0);
    lv_obj_set_style_border_width(table, 0, 0);
    lv_obj_set_style_radius(table, 8, 0);
    lv_obj_set_style_pad_all(table, 0, 0); // Remove padding
    lv_obj_add_flag(table, LV_OBJ_FLAG_SCROLL_ELASTIC); // Ensure scrollable

    lv_table_set_col_width(table, 0, 80);  // Step num
    lv_table_set_col_width(table, 1, 180); // Temp
    lv_table_set_col_width(table, 2, 220); // Rate
    lv_table_set_col_width(table, 3, 180); // Hold
    lv_table_set_col_width(table, 4, 60);  // Edit

    // Header
    lv_table_set_cell_value(table, 0, 0, "КРОК");
    lv_table_set_cell_value(table, 0, 1, "ТЕМПЕРАТУРА");
    lv_table_set_cell_value(table, 0, 2, "ШВИДКІСТЬ");
    lv_table_set_cell_value(table, 0, 3, "ЧАС УТРИМКИ");
    lv_table_set_cell_value(table, 0, 4, ""); // Empty for edit icon

    // Set header style
    lv_obj_add_style(table, &style_table_header, LV_PART_ITEMS);

    // Body
    if (!thermalCtrl.getActiveSchedule().empty()) {
        uint16_t row_idx = 1;
        for (const auto& step : thermalCtrl.getActiveSchedule()) {
            // Step Number
            char num_buf[16];
            snprintf(num_buf, sizeof(num_buf), ":: %d", row_idx);
            lv_table_set_cell_value(table, row_idx, 0, num_buf);

            // Type
            char type_buf[16];
            snprintf(type_buf, sizeof(type_buf), "%s", step.type == 0 ? "RAMP" : "HOLD");
            lv_table_set_cell_value(table, row_idx, 1, type_buf);

            // Value
            char value_buf[24];
            snprintf(value_buf, sizeof(value_buf), "%.1f", step.value);
            lv_table_set_cell_value(table, row_idx, 2, value_buf);

            // Rate
            char rate_buf[16];
            snprintf(rate_buf, sizeof(rate_buf), "%.1f", step.rate);
            lv_table_set_cell_value(table, row_idx, 3, rate_buf);

            // Edit Icon
            lv_table_set_cell_value(table, row_idx, 4, LV_SYMBOL_EDIT);

            row_idx++;
        }
        lv_table_set_row_cnt(table, row_idx);
    } else {
        lv_table_set_row_cnt(table, 1);
    }
}
