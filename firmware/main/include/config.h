/*
  Hardware Configuration for Trae Kiln Controller
  Board: Freenove ESP32-S3 WROOM (or compatible)
*/
#pragma once

#include "driver/gpio.h"

// --- PINOUT CONFIGURATION ---

// 1. DISPLAY (ST7796S via LovyanGFX)
#ifndef TFT_SCLK
#define TFT_SCLK        GPIO_NUM_12
#endif
#ifndef TFT_MOSI
#define TFT_MOSI        GPIO_NUM_11
#endif
#ifndef TFT_MISO
#define TFT_MISO        GPIO_NUM_13
#endif
#ifndef TFT_DC
#define TFT_DC          GPIO_NUM_10
#endif
#ifndef TFT_CS
#define TFT_CS          GPIO_NUM_9
#endif
#ifndef TFT_RST
#define TFT_RST         GPIO_NUM_3
#endif
#ifndef TFT_BL
#define TFT_BL          GPIO_NUM_18
#endif

#ifndef TOUCH_CS
#define TOUCH_CS        GPIO_NUM_4
#endif
#ifndef TOUCH_IRQ
#define TOUCH_IRQ       GPIO_NUM_NC
#endif

// 2. THERMOCOUPLE (MAX6675) - Software SPI
#define MAXCLK          GPIO_NUM_41
#define MAXCS           GPIO_NUM_42
#define MAXDO           GPIO_NUM_40 

// 3. RELAYS
#define SSR_ZONE1_PIN       GPIO_NUM_21
#define SAFETY_RELAY_PIN    GPIO_NUM_47

// 4. EXTRAS
#define FAN_PIN             GPIO_NUM_8
#define BUZZER_PIN          GPIO_NUM_48

// --- General Config ---
#define FIRMWARE_VERSION    "2.5.1"
#define DEVICE_NAME         "Trae Kiln Controller"

// --- Safety Limits ---
#define MAX_TEMP_SAFETY     1300.0f
#define MAX_TEMP_RISE_RATE  500.0f
#define WATCHDOG_TIMEOUT_MS 60000

// --- PID Defaults ---
#define PID_WINDOW_SIZE     2000    // 2 seconds for SSR
#define PID_KP              22.2f
#define PID_KI              1.06f
#define PID_KD              116.55f

// --- WiFi Configuration ---
#define CONFIG_WIFI_SSID     "Kiln_Controller"
#define CONFIG_WIFI_PASSWORD "kiln123456"

// --- File System Paths ---
#define LITTLEFS_BASE_PATH   "/littlefs"
#define CONFIG_FILE         "/littlefs/config.json"
#define SCHEDULES_DIR       "/littlefs/schedules"
#define LOGS_DIR            "/littlefs/logs"
