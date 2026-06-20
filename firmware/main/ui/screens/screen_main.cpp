#include "screens.h"
#include "screen_header.h"
#include "theme.h"
#include "kiln_control/thermal_control.h"
#include "picopixel/images.h"
#include <ctime>
#include <cstring>
#include <cstdio>
#include <esp_timer.h>

static lv_obj_t* time_label = nullptr;
static lv_obj_t* current_temp_label = nullptr;
static lv_obj_t* target_temp_label = nullptr;
static lv_obj_t* state_label = nullptr;
static lv_obj_t* program_name_label = nullptr;
static lv_obj_t* step_label = nullptr;
static lv_obj_t* current_temp_value = nullptr;
static lv_obj_t* target_temp_value = nullptr;
static lv_obj_t* heating_rate_value = nullptr;
static lv_obj_t* voltage_label = nullptr;
static lv_obj_t* current_label = nullptr;
static lv_obj_t* power_label = nullptr;
static lv_obj_t* consumption_label = nullptr;
static lv_obj_t* progress_bar = nullptr;
static lv_obj_t* step_progress_bar = nullptr;
static lv_obj_t* start_btn = nullptr;
static lv_obj_t* pause_btn = nullptr;
static lv_obj_t* stop_btn = nullptr;

static uint64_t g_last_clock_update_ms = 0;
static uint64_t g_last_state_update_ms = 0;

static char g_last_time_str[32] = {0};
static char g_last_temp_str[16] = {0};
static char g_last_target_str[16] = {0};
static char g_last_program_str[64] = {0};
static char g_last_step_str[32] = {0};
static char g_last_state_str[32] = {0};

extern ThermalController thermalCtrl;

static void start_event_cb(lv_event_t* e) {
    (void)e;
    thermalCtrl.start();
}

static void pause_event_cb(lv_event_t* e) {
    (void)e;
    thermalCtrl.stop("Pause");
}

static void stop_event_cb(lv_event_t* e) {
    (void)e;
    thermalCtrl.stop("Stop");
}

static void programs_event_cb(lv_event_t* e) {
    (void)e;
    // screens_request_show(SCREEN_ID_PROGRAMS);
}

static void settings_event_cb(lv_event_t* e) {
    (void)e;
    // screens_request_show(SCREEN_ID_SETTINGS);
}

void screen_main_create() {
    lv_obj_t* scr = lv_obj_create(NULL);
    screen_prepare_root(scr);

    ScreenHeaderRefs header = screen_header_create(scr, settings_event_cb);
    time_label = header.time_label;

    lv_obj_t* temp_panel = lv_obj_create(scr);
    lv_obj_set_size(temp_panel, 385, 300);
    lv_obj_set_pos(temp_panel, 10, 60);
    lv_obj_set_style_bg_color(temp_panel, COL_BG_CARD, 0);
    lv_obj_set_style_radius(temp_panel, RADIUS_CARD, 0);
    lv_obj_set_style_border_width(temp_panel, 1, 0);
    lv_obj_set_style_border_color(temp_panel, COL_BORDER, 0);
    lv_obj_set_style_bg_opa(temp_panel, 206, 0);

    state_label = lv_label_create(temp_panel);
    lv_label_set_text(state_label, "ОЧІКУВАННЯ");
    lv_obj_set_style_text_font(state_label, FONT_TINY, 0);
    lv_obj_set_style_text_color(state_label, COL_ACCENT, 0);
    lv_obj_set_pos(state_label, 23, 6);

    lv_obj_t* state_panel = lv_obj_create(temp_panel);
    lv_obj_set_size(state_panel, 148, 30);
    lv_obj_set_pos(state_panel, 15, 15);
    lv_obj_set_style_bg_opa(state_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state_panel, 0, 0);
    lv_obj_set_style_radius(state_panel, 50, 0);
    lv_obj_set_style_outline_color(state_panel, COL_ACCENT, 0);
    lv_obj_set_style_outline_width(state_panel, 2, 0);
    lv_obj_set_style_outline_pad(state_panel, 0, 0);

    lv_obj_t* current_temp_title = lv_label_create(temp_panel);
    lv_label_set_text(current_temp_title, "Поточна температура");
    lv_obj_set_style_text_font(current_temp_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(current_temp_title, COL_TEXT, 0);
    lv_obj_set_pos(current_temp_title, 15, 55);

    current_temp_value = lv_label_create(temp_panel);
    lv_label_set_text(current_temp_value, "27,5");
    lv_obj_set_style_text_font(current_temp_value, FONT_HUGE, 0);
    lv_obj_set_style_text_color(current_temp_value, COL_TEXT, 0);
    lv_obj_set_pos(current_temp_value, 15, 80);

    lv_obj_t* temp_unit = lv_label_create(temp_panel);
    lv_label_set_text(temp_unit, "°C");
    lv_obj_set_style_text_font(temp_unit, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(temp_unit, 232, 94);

    lv_obj_t* target_title = lv_label_create(temp_panel);
    lv_label_set_text(target_title, "Цільова температура");
    lv_obj_set_style_text_font(target_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(target_title, COL_TEXT_DIM, 0);
    lv_obj_set_pos(target_title, 15, 210);

    target_temp_value = lv_label_create(temp_panel);
    lv_label_set_text(target_temp_value, "100");
    lv_obj_set_style_text_font(target_temp_value, FONT_XXL, 0);
    lv_obj_set_style_text_color(target_temp_value, COL_ACCENT, 0);
    lv_obj_set_pos(target_temp_value, 15, 230);

    lv_obj_t* heating_title = lv_label_create(temp_panel);
    lv_label_set_text(heating_title, "Швидкість нагрівання");
    lv_obj_set_style_text_font(heating_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(heating_title, COL_TEXT_DIM, 0);
    lv_obj_set_pos(heating_title, 203, 210);

    heating_rate_value = lv_label_create(temp_panel);
    lv_label_set_text(heating_rate_value, "50");
    lv_obj_set_style_text_font(heating_rate_value, FONT_XXL, 0);
    lv_obj_set_pos(heating_rate_value, 204, 230);

    lv_obj_t* heating_unit = lv_label_create(temp_panel);
    lv_label_set_text(heating_unit, "°С/год");
    lv_obj_set_style_text_font(heating_unit, FONT_TINY, 0);
    lv_obj_set_pos(heating_unit, 261, 241);

    progress_bar = lv_bar_create(temp_panel);
    lv_obj_set_size(progress_bar, 354, 10);
    lv_obj_set_pos(progress_bar, 16, 271);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_bar_set_value(progress_bar, 50, LV_ANIM_OFF);
    lv_obj_set_style_radius(progress_bar, 20, 0);
    lv_obj_set_style_bg_color(progress_bar, COL_TEXT_DIM, 0);
    lv_obj_set_style_bg_color(progress_bar, COL_ACCENT, LV_PART_INDICATOR);

    lv_obj_t* program_panel = lv_obj_create(scr);
    lv_obj_set_size(program_panel, 385, 177);
    lv_obj_set_pos(program_panel, 405, 63);
    lv_obj_set_style_bg_color(program_panel, COL_BG_CARD, 0);
    lv_obj_set_style_radius(program_panel, RADIUS_CARD, 0);
    lv_obj_set_style_border_width(program_panel, 1, 0);
    lv_obj_set_style_border_color(program_panel, COL_BORDER, 0);
    lv_obj_set_style_bg_opa(program_panel, 224, 0);

    program_name_label = lv_label_create(program_panel);
    lv_label_set_text(program_name_label, "Немає програми");
    lv_obj_set_style_text_font(program_name_label, FONT_XXL, 0);
    lv_obj_set_style_text_color(program_name_label, COL_TEXT, 0);
    lv_obj_set_pos(program_name_label, 13, 38);

    lv_obj_t* program_subtitle = lv_label_create(program_panel);
    lv_label_set_text(program_subtitle, "Вибрана програма");
    lv_obj_set_style_text_font(program_subtitle, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(program_subtitle, COL_ACCENT, 0);
    lv_obj_set_pos(program_subtitle, 13, 12);

    lv_obj_t* step_title = lv_label_create(program_panel);
    lv_label_set_text(step_title, "Крок");
    lv_obj_set_style_text_font(step_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(step_title, COL_TEXT_DIM, 0);
    lv_obj_set_pos(step_title, 13, 91);

    step_label = lv_label_create(program_panel);
    lv_label_set_text(step_label, "1/10");
    lv_obj_set_style_text_font(step_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(step_label, COL_TEXT_DIM, 0);
    lv_obj_set_pos(step_label, 344, 89);

    step_progress_bar = lv_bar_create(program_panel);
    lv_obj_set_size(step_progress_bar, 358, 11);
    lv_obj_set_pos(step_progress_bar, 13, 127);
    lv_bar_set_range(step_progress_bar, 1, 10);
    lv_bar_set_value(step_progress_bar, 2, LV_ANIM_OFF);
    lv_obj_set_style_radius(step_progress_bar, 2, 0);
    lv_obj_set_style_bg_color(step_progress_bar, COL_TEXT_DIM, 0);
    lv_obj_set_style_bg_color(step_progress_bar, COL_ACCENT, LV_PART_INDICATOR);

    lv_obj_t* arrow_img = lv_img_create(program_panel);
    lv_obj_set_size(arrow_img, 32, 32);
    lv_obj_set_pos(arrow_img, 340, 37);
    lv_obj_set_style_img_recolor_opa(arrow_img, 255, 0);
    lv_obj_set_style_img_recolor(arrow_img, COL_TEXT, 0);
    lv_img_set_src(arrow_img, &free_icon_right_arrow_271228_png);
    lv_obj_add_flag(arrow_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(arrow_img, programs_event_cb, LV_EVENT_CLICKED, nullptr);

    start_btn = lv_btn_create(scr);
    lv_obj_set_size(start_btn, 145, 110);
    lv_obj_set_pos(start_btn, 405, 250);
    lv_obj_set_style_bg_color(start_btn, COL_AMBER, 0);
    lv_obj_set_style_radius(start_btn, RADIUS_BTN, 0);
    lv_obj_set_style_bg_opa(start_btn, 255, 0);
    lv_obj_add_event_cb(start_btn, start_event_cb, LV_EVENT_CLICKED, nullptr);

    pause_btn = lv_btn_create(scr);
    lv_obj_set_size(pause_btn, 110, 110);
    lv_obj_set_pos(pause_btn, 560, 250);
    lv_obj_set_style_bg_color(pause_btn, COL_BG_CARD, 0);
    lv_obj_set_style_radius(pause_btn, RADIUS_BTN, 0);
    lv_obj_set_style_border_width(pause_btn, 1, 0);
    lv_obj_set_style_border_color(pause_btn, COL_BORDER, 0);
    lv_obj_add_event_cb(pause_btn, pause_event_cb, LV_EVENT_CLICKED, nullptr);

    stop_btn = lv_btn_create(scr);
    lv_obj_set_size(stop_btn, 110, 110);
    lv_obj_set_pos(stop_btn, 680, 250);
    lv_obj_set_style_bg_color(stop_btn, COL_BTN_DANGER, 0);
    lv_obj_set_style_radius(stop_btn, RADIUS_BTN, 0);
    lv_obj_set_style_border_width(stop_btn, 1, 0);
    lv_obj_set_style_border_color(stop_btn, COL_RED, 0);
    lv_obj_add_event_cb(stop_btn, stop_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* start_label = lv_label_create(start_btn);
    lv_label_set_text(start_label, "");
    lv_obj_center(start_label);

    lv_obj_t* pause_label = lv_label_create(pause_btn);
    lv_label_set_text(pause_label, "Пауза");
    lv_obj_set_style_text_font(pause_label, FONT_TINY, 0);
    lv_obj_center(pause_label);

    lv_obj_t* stop_label = lv_label_create(stop_btn);
    lv_label_set_text(stop_label, "Стоп");
    lv_obj_set_style_text_font(stop_label, FONT_TINY, 0);
    lv_obj_center(stop_label);

    lv_obj_t* network_panel = lv_obj_create(scr);
    lv_obj_set_size(network_panel, 780, 99);
    lv_obj_set_pos(network_panel, 10, 371);
    lv_obj_set_style_bg_color(network_panel, COL_BG_CARD, 0);
    lv_obj_set_style_radius(network_panel, RADIUS_CARD, 0);
    lv_obj_set_style_border_width(network_panel, 1, 0);
    lv_obj_set_style_border_color(network_panel, COL_BORDER, 0);

    lv_obj_t* network_title = lv_label_create(network_panel);
    lv_label_set_text(network_title, "Електрична мережа");
    lv_obj_set_style_text_font(network_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(network_title, COL_TEXT_DIM, 0);
    lv_obj_set_pos(network_title, 14, 11);

    voltage_label = lv_label_create(network_panel);
    lv_label_set_text(voltage_label, "230 V");
    lv_obj_set_style_text_font(voltage_label, FONT_TINY, 0);
    lv_obj_set_style_text_color(voltage_label, COL_TEXT, 0);
    lv_obj_set_pos(voltage_label, 55, 60);

    lv_obj_t* voltage_title = lv_label_create(network_panel);
    lv_label_set_text(voltage_title, "Напруга");
    lv_obj_set_style_text_font(voltage_title, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(voltage_title, 55, 38);

    current_label = lv_label_create(network_panel);
    lv_label_set_text(current_label, "2.4 A");
    lv_obj_set_style_text_font(current_label, FONT_TINY, 0);
    lv_obj_set_style_text_color(current_label, COL_TEXT, 0);
    lv_obj_set_pos(current_label, 241, 59);

    lv_obj_t* current_title = lv_label_create(network_panel);
    lv_label_set_text(current_title, "Струм");
    lv_obj_set_style_text_font(current_title, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(current_title, 241, 38);

    power_label = lv_label_create(network_panel);
    lv_label_set_text(power_label, "552 W");
    lv_obj_set_style_text_font(power_label, FONT_TINY, 0);
    lv_obj_set_style_text_color(power_label, COL_TEXT, 0);
    lv_obj_set_pos(power_label, 436, 59);

    lv_obj_t* power_title = lv_label_create(network_panel);
    lv_label_set_text(power_title, "Потужність");
    lv_obj_set_style_text_font(power_title, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(power_title, 436, 38);

    consumption_label = lv_label_create(network_panel);
    lv_label_set_text(consumption_label, "1.25 kWh");
    lv_obj_set_style_text_font(consumption_label, FONT_TINY, 0);
    lv_obj_set_style_text_color(consumption_label, COL_TEXT, 0);
    lv_obj_set_pos(consumption_label, 631, 58);

    lv_obj_t* consumption_title = lv_label_create(network_panel);
    lv_label_set_text(consumption_title, "Споживання");
    lv_obj_set_style_text_font(consumption_title, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(consumption_title, 631, 38);
}

void screen_main_update() {
    const uint64_t now = (uint64_t)(esp_timer_get_time() / 1000ULL);

    if (now - g_last_clock_update_ms >= 1000) {
        g_last_clock_update_ms = now;
        if (time_label) {
            time_t ts = time(nullptr);
            struct tm tmv {};
            localtime_r(&ts, &tmv);
            char buf[64];
            strftime(buf, sizeof(buf), "%H:%M %d.%m.%Y", &tmv);
            if (strcmp(g_last_time_str, buf) != 0) {
                lv_label_set_text(time_label, buf);
                strncpy(g_last_time_str, buf, sizeof(g_last_time_str) - 1);
            }
        }
    }

    if (now - g_last_state_update_ms >= 500) {
        g_last_state_update_ms = now;

        KilnState st = thermalCtrl.getState();

        if (current_temp_value) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.1f", st.currentTemp);
            if (strcmp(g_last_temp_str, buf) != 0) {
                lv_label_set_text(current_temp_value, buf);
                strncpy(g_last_temp_str, buf, sizeof(g_last_temp_str) - 1);
            }
        }

        if (target_temp_value) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.0f", st.targetTemp);
            if (strcmp(g_last_target_str, buf) != 0) {
                lv_label_set_text(target_temp_value, buf);
                strncpy(g_last_target_str, buf, sizeof(g_last_target_str) - 1);
            }
        }

        if (program_name_label) {
            char name[64];
            if (st.totalSteps > 0) {
                snprintf(name, sizeof(name), "Кроків: %d", st.totalSteps);
            } else {
                snprintf(name, sizeof(name), "Немає програми");
            }
            if (strcmp(g_last_program_str, name) != 0) {
                lv_label_set_text(program_name_label, name);
                strncpy(g_last_program_str, name, sizeof(g_last_program_str) - 1);
            }
        }

        if (step_label) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d/%d", st.currentStep + 1, st.totalSteps > 0 ? st.totalSteps : 1);
            if (strcmp(g_last_step_str, buf) != 0) {
                lv_label_set_text(step_label, buf);
                strncpy(g_last_step_str, buf, sizeof(g_last_step_str) - 1);
            }
        }

        if (state_label) {
            const char* state_text = "ОЧІКУВАННЯ";
            switch (st.status) {
                case KILN_RUNNING: state_text = "ВИПАЛ"; break;
                case KILN_HOLD: state_text = "УТРИМКА"; break;
                case KILN_COOLING: state_text = "ОХОЛОДЖЕННЯ"; break;
                case KILN_COMPLETE: state_text = "ГОТОВО"; break;
                case KILN_FAULT: state_text = "ПОМИЛКА"; break;
                case KILN_PAUSED: state_text = "ПАУЗА"; break;
                case KILN_TUNING: state_text = "АВТОТЮН"; break;
                case KILN_IDLE:
                default: state_text = "ОЧІКУВАННЯ"; break;
            }
            if (strcmp(g_last_state_str, state_text) != 0) {
                lv_label_set_text(state_label, state_text);
                strncpy(g_last_state_str, state_text, sizeof(g_last_state_str) - 1);
                if (st.status == KILN_FAULT) {
                    lv_obj_set_style_text_color(state_label, COL_RED, 0);
                } else if (st.isFiring) {
                    lv_obj_set_style_text_color(state_label, COL_RED, 0);
                } else {
                    lv_obj_set_style_text_color(state_label, COL_ACCENT, 0);
                }
            }
        }
    }
}
