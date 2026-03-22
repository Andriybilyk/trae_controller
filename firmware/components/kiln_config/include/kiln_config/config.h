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

// Display SPI frequency. Typical stable range for ST7796S is ~20-40 MHz.
// If you see artifacts/tearing, try 26 MHz or 20 MHz.
#ifndef TFT_SPI_CLOCK_HZ
#define TFT_SPI_CLOCK_HZ (40 * 1000 * 1000)
#endif
// Number of full display lines per SPI DMA transaction chunk.
// Larger chunks improve throughput; smaller chunks can reduce frame-time jitter.
#ifndef TFT_SPI_DMA_LINES
#define TFT_SPI_DMA_LINES 12
#endif
// SPI queued transactions for display writes.
#ifndef TFT_SPI_QUEUE_SIZE
#define TFT_SPI_QUEUE_SIZE 10
#endif

#ifndef TOUCH_CS
#define TOUCH_CS        GPIO_NUM_4
#endif
#ifndef TOUCH_IRQ
#define TOUCH_IRQ       GPIO_NUM_5
#endif

// Touch controller (XPT2046) SPI pins.
// MSP4021 exposes separate pins: T_CLK, T_DIN, T_DO, T_CS, T_IRQ.
// Default to the display SPI pins, but override if your wiring uses separate pins.
#ifndef TOUCH_SCLK
#define TOUCH_SCLK      TFT_SCLK
#endif
#ifndef TOUCH_MOSI
#define TOUCH_MOSI      TFT_MOSI
#endif
#ifndef TOUCH_MISO
#define TOUCH_MISO      TFT_MISO
#endif

// Touch SPI frequency. XPT2046 is typically reliable at 1-2 MHz.
#ifndef TOUCH_SPI_CLOCK_HZ
#define TOUCH_SPI_CLOCK_HZ (1 * 1000 * 1000)
#endif

// Target UI frame rate for Slint render loop.
#ifndef UI_TARGET_FPS
#define UI_TARGET_FPS 30
#endif

// 2. THERMOCOUPLE (MAX6675) - Software SPI
#define MAXCLK          GPIO_NUM_40
#define MAXCS           GPIO_NUM_42
#define MAXDO           GPIO_NUM_41

// 3. RELAYS
#define SSR_ZONE1_PIN       GPIO_NUM_21
#define SAFETY_RELAY_PIN    GPIO_NUM_47

// 4. EXTRAS
#define FAN_PIN             GPIO_NUM_8
#define BUZZER_PIN          GPIO_NUM_48

// --- General Config ---
#define FIRMWARE_VERSION    "2.5.1"
#define DEVICE_NAME         "Trae Kiln Controller"

// --- Time / NTP ---
// TZ format: https://man7.org/linux/man-pages/man3/tzset.3.html
#define TIMEZONE_TZ         "EET-2EEST,M3.5.0/3,M10.5.0/4"
#define SNTP_SERVER_1       "pool.ntp.org"

// --- Safety Limits ---
#define MAX_TEMP_SAFETY     1300.0f
#define MAX_TEMP_RISE_RATE  500.0f
#define WATCHDOG_TIMEOUT_MS 60000

// --- Sensor Plausibility ---
#define SENSOR_TEMP_MIN_C   (-50.0f)
#define SENSOR_TEMP_MAX_C   (2000.0f)

// --- Fault Reset Policy ---
// Only allow clearing a latched fault when temperature is below this threshold.
#define FAULT_CLEAR_MAX_TEMP_C  (200.0f)
// Auto-clear sensor fault after this stable healthy time (ms).
#define SENSOR_FAULT_AUTO_CLEAR_MS (3000)

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
