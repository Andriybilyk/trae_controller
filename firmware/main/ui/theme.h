#pragma once
#include "lvgl.h"

#define COL_BG          lv_color_hex(0xFF090C10)
#define COL_BG_CARD     lv_color_hex(0xFF0E131F)
#define COL_BG_HEADER   lv_color_hex(0xFF0E131F)
#define COL_BTN_BG      lv_color_hex(0xFF0E131F)
#define COL_BTN_ACTIVE  lv_color_hex(0xFFB77100)
#define COL_BTN_DANGER  lv_color_hex(0xFF421212)

#define COL_ACCENT      lv_color_hex(0xFF00B761)
#define COL_AMBER       lv_color_hex(0xFFB77100)
#define COL_RED         lv_color_hex(0xFFFF3B30)
#define COL_GREEN       lv_color_hex(0xFF00B761)

#define COL_TEXT        lv_color_hex(0xFFFFFFFF)
#define COL_TEXT_DIM    lv_color_hex(0xFF7A8B9E)
#define COL_TEXT_VDIM   lv_color_hex(0xFF4A4A4A)

#define COL_BORDER      lv_color_hex(0xFF1C2538)
#define COL_BORDER_ACT  lv_color_hex(0xFF00B761)

#define SCREEN_W        800
#define SCREEN_H        480
#define HEADER_H        50

#define RADIUS_BTN      10
#define RADIUS_CARD     10

#define PAD_X           16
#define PAD_X2          12

#define FONT_TINY       &lv_font_montserrat_14
#define FONT_SMALL      &lv_font_montserrat_14
#define FONT_NORMAL      &lv_font_montserrat_14
#define FONT_MED        &lv_font_montserrat_14
#define FONT_SUBTITLE   &lv_font_montserrat_14
#define FONT_LARGE      &lv_font_montserrat_14
#define FONT_XL         &lv_font_montserrat_14
#define FONT_XXL        &lv_font_montserrat_14
#define FONT_HUGE       &lv_font_montserrat_14

void theme_init();
