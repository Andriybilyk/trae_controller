#include "screens.h"
#include "theme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <ctime>
#include <cstdio>
#include <esp_timer.h>

static lv_obj_t* bootStatusLabel = nullptr;
static lv_obj_t* bootProgressBar = nullptr;
static lv_obj_t* bootPercentLabel = nullptr;
static uint64_t g_last_clock_update_ms = 0;

static const char* get_state_text(int percent) {
    if (percent < 10) return "Ініціалізація...";
    if (percent < 30) return "Завантаження дисплея...";
    if (percent < 50) return "Підключення датчиків...";
    if (percent < 70) return "Завантаження програм...";
    if (percent < 90) return "Підготовка UI...";
    return "Готово!";
}

static void update_clock() {
    const uint64_t now = (uint64_t)(esp_timer_get_time() / 1000ULL);
    if (now - g_last_clock_update_ms < 1000) return;
    g_last_clock_update_ms = now;

    if (!bootStatusLabel) return;

    time_t ts = time(nullptr);
    struct tm tmv {};
    localtime_r(&ts, &tmv);

    char buf[64];
    strftime(buf, sizeof(buf), "%H:%M %d.%m.%Y", &tmv);
    lv_label_set_text(bootStatusLabel, buf);
}

void screen_boot_create() {
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "KilnPro");
    lv_obj_set_style_text_font(title, FONT_NORMAL, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_set_pos(title, 64, 14);

    lv_obj_t* logo_panel = lv_obj_create(scr);
    lv_obj_set_size(logo_panel, 32, 32);
    lv_obj_set_pos(logo_panel, 25, 13);
    lv_obj_set_style_bg_color(logo_panel, COL_ACCENT, 0);
    lv_obj_set_style_radius(logo_panel, 5, 0);
    lv_obj_set_style_border_width(logo_panel, 0, 0);

    bootProgressBar = lv_bar_create(scr);
    lv_obj_set_size(bootProgressBar, SCREEN_W - 40, 10);
    lv_obj_set_pos(bootProgressBar, 20, SCREEN_H - 80);
    lv_bar_set_range(bootProgressBar, 0, 100);
    lv_bar_set_value(bootProgressBar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bootProgressBar, 5, 0);
    lv_obj_set_style_bg_color(bootProgressBar, COL_BORDER, 0);
    lv_obj_set_style_bg_color(bootProgressBar, COL_ACCENT, LV_PART_INDICATOR);

    bootPercentLabel = lv_label_create(scr);
    lv_label_set_text(bootPercentLabel, "0%");
    lv_obj_set_style_text_font(bootPercentLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bootPercentLabel, COL_TEXT_DIM, 0);
    lv_obj_set_pos(bootPercentLabel, SCREEN_W - 80, SCREEN_H - 100);

    bootStatusLabel = lv_label_create(scr);
    lv_label_set_text(bootStatusLabel, get_state_text(0));
    lv_obj_set_style_text_font(bootStatusLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bootStatusLabel, COL_TEXT_DIM, 0);
    lv_obj_set_pos(bootStatusLabel, 20, SCREEN_H - 50);

    lv_obj_t* time_label = lv_label_create(scr);
    lv_label_set_text(time_label, "");
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_label, COL_TEXT, 0);
    lv_obj_set_pos(time_label, SCREEN_W - 200, 14);
}

void screen_boot_update(int percent, const char* status) {
    if (bootProgressBar) {
        lv_bar_set_value(bootProgressBar, percent, LV_ANIM_OFF);
    }
    if (bootPercentLabel) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", percent);
        lv_label_set_text(bootPercentLabel, buf);
    }
    if (bootStatusLabel && status) {
        lv_label_set_text(bootStatusLabel, status);
    }
}
