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
#if CONFIG_TC_BOARD_NEW_P4
// New P4 module wiring (user-provided):
// SCK -> GPIO33, CS -> GPIO30, DO(SO) -> GPIO31
#define MAXCLK          GPIO_NUM_33
#define MAXCS           GPIO_NUM_30
#define MAXDO           GPIO_NUM_31
#else
#define MAXCLK          GPIO_NUM_40
#define MAXCS           GPIO_NUM_42
#define MAXDO           GPIO_NUM_41
#endif

// 3. RELAYS
#if CONFIG_TC_BOARD_NEW_P4
// NewP4 expansion board:
// - SAFETY_RELAY drives contactor relay coil on GPIO35 (ON while firing)
// - SSR output is not used in this build
#define SSR_ZONE1_PIN       GPIO_NUM_NC
#define SAFETY_RELAY_PIN    GPIO_NUM_35
#else
#define SSR_ZONE1_PIN       GPIO_NUM_21
#define SAFETY_RELAY_PIN    GPIO_NUM_47
#endif

// 4. EXTRAS
#if CONFIG_TC_BOARD_NEW_P4
// SparkFun MOSFET Power Switch CTL is active-low; use dedicated fan control pin.
#define FAN_PIN             GPIO_NUM_29
#define FAN_ACTIVE_LOW      1
#define BUZZER_PIN          GPIO_NUM_28
#else
#define FAN_PIN             GPIO_NUM_8
#define FAN_ACTIVE_LOW      0
#define BUZZER_PIN          GPIO_NUM_48
#endif

#ifndef RTC_I2C_PORT
#define RTC_I2C_PORT        0
#endif
#ifndef RTC_I2C_SDA
#define RTC_I2C_SDA         GPIO_NUM_6
#endif
#ifndef RTC_I2C_SCL
#define RTC_I2C_SCL         GPIO_NUM_7
#endif
#ifndef RTC_I2C_FREQ_HZ
#define RTC_I2C_FREQ_HZ     100000
#endif
#ifndef RTC_DS3231_ADDR
#define RTC_DS3231_ADDR     0x68
#endif

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
