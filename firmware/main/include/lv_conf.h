/**
 * @file lv_conf.h
 * Configuration file for v8.3.0
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>
#include "sdkconfig.h"

/*====================
   COLOR SETTINGS
 *====================*/

/*Color depth: 1 (1 byte per pixel), 8 (RGB332), 16 (RGB565), 32 (ARGB8888)*/
#define LV_COLOR_DEPTH 16

/*Swap the 2 bytes of RGB565 color. Useful if the display has an 8-bit interface (e.g. SPI)*/
#define LV_COLOR_16_SWAP 0

/*1: Enable screen transparency.*/
#define LV_COLOR_SCREEN_TRANSP 0

/*=========================
   MEMORY SETTINGS
 *=========================*/

#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CUSTOM
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/*====================
   HAL SETTINGS
 *====================*/

/*1: Use a custom tick source that uses the system time instead of millis()*/
#define LV_TICK_CUSTOM 1
#define LV_ATTRIBUTE_FAST_MEM
#if LV_TICK_CUSTOM
    #define LV_TICK_CUSTOM_INCLUDE "esp_timer.h"         /*Header for the system time function*/
    #define LV_TICK_CUSTOM_SYS_TIME_EXPR ((esp_timer_get_time() / 1000))     /*Expression evaluating to current system time in ms*/
#endif

/*====================
   FEATURE CONFIGURATION
 *====================*/

/*-------------
 * Drawing
 *-----------*/

/*Enable complex draw engine*/
#if CONFIG_IDF_TARGET_ESP32P4
    #define LV_DRAW_COMPLEX 1
#else
    #define LV_DRAW_COMPLEX 0
#endif

/*Default display refresh period. LVG will redraw changed areas with this period time*/
#if CONFIG_IDF_TARGET_ESP32P4
    #define LV_DISP_DEF_REFR_PERIOD 16      /*[ms] - ~60FPS for P4 smoothness*/
#else
    #define LV_DISP_DEF_REFR_PERIOD 40      /*[ms] - reduce CPU load for SPI displays*/
#endif

/*Input device read period in milliseconds*/
#if CONFIG_IDF_TARGET_ESP32P4
    #define LV_INDEV_DEF_READ_PERIOD 16     /*[ms]*/
#else
    #define LV_INDEV_DEF_READ_PERIOD 30     /*[ms]*/
#endif

/*-------------
 * Others
 *-----------*/

/*1: Enable the log module*/
#define LV_USE_LOG 1
#if LV_USE_LOG
    #define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF 1
    #define LV_LOG_USE_TIMESTAMP 1
#endif

/*====================
   FONT USAGE
 *====================*/

/*Montserrat fonts enabled for PicoPixel UI*/
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_30 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_34 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_38 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_42 1
#define LV_FONT_MONTSERRAT_44 1
#define LV_FONT_MONTSERRAT_46 1
#define LV_FONT_MONTSERRAT_48 1

#define LV_USE_QRCODE 1

/*====================
   THEME USAGE
 *====================*/
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1
#if CONFIG_IDF_TARGET_ESP32P4
    #define LV_THEME_DEFAULT_TRANSITION_TIME 80
#else
    #define LV_THEME_DEFAULT_TRANSITION_TIME 0
#endif

#endif /*LV_CONF_H*/
