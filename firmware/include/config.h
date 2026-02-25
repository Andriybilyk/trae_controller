#ifndef CONFIG_H
#define CONFIG_H

// --- Hardware Pins (ESP32-S3 default) ---
// Thermocouple (MAX31855) - Software SPI to avoid conflict with Display
#define MAXDO   12
#define MAXCS   10
#define MAXCLK  11

// Display (TFT_eSPI Hardware SPI)
// SCK=18, MISO=19, MOSI=23, CS=5, DC=2, RST=4
#define TOUCH_CS 21

// SSR Pins for Zones 1-3
#define SSR_ZONE1_PIN 15
#define SSR_ZONE2_PIN 16
#define SSR_ZONE3_PIN 17

// Door Switch
#define DOOR_SWITCH_PIN 14

// Fan Control (MOSFET)
#define FAN_PIN 13

// Safety Features
#define SAFETY_RELAY_PIN 6  // Mechanical Relay to cut power to heaters
#define BUZZER_PIN 7        // Active Buzzer for alarms

// Display (configured in platformio.ini via build flags for TFT_eSPI)
// Touch CS usually needs definition if using XPT2046
#define TOUCH_CS 21

// --- WiFi Configuration ---
// In production, use WiFiManager or store in LittleFS
#define DEFAULT_WIFI_SSID "Kiln_Controller"
#define DEFAULT_WIFI_PASS "kiln123456"

// --- Safety Limits ---
#define MAX_TEMP_SAFETY 1300.0 // Celsius
#define MAX_TEMP_RISE_RATE 500.0 // Degrees per hour sanity check
#define WATCHDOG_TIMEOUT_MS 10000

// --- PID Defaults ---
#define PID_KP 200.0
#define PID_KI 0.5
#define PID_KD 100.0
#define PID_WINDOW_SIZE 2000 // 2 seconds window for SSR PWM

// --- File System Paths ---
#define CONFIG_FILE "/config.json"
#define SCHEDULES_DIR "/schedules"
#define LOGS_DIR "/logs"

#endif
