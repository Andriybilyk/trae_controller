/*
  Hardware Configuration for Trae Kiln Controller
*/
#pragma once

// --- Thermocouple Pins (SPI) ---
// These are standard SPI pins for most ESP32-S3 boards
#define MAXDO   11  // SPI MISO (Master In, Slave Out)
#define MAXCS   12  // SPI Chip Select (You can change this one)
#define MAXCLK  13  // SPI Clock

// --- PID & SSR Configuration ---
#define SSR_ZONE1_PIN       1       // Main SSR for heater
#define SSR_ZONE2_PIN       2       // Optional 2nd zone
#define SSR_ZONE3_PIN       3       // Optional 3rd zone

#define PID_WINDOW_SIZE     2000    // PID cycle time in milliseconds (e.g., 2000ms = 2s)
#define PID_KP              22.2    // Default Proportional Gain
#define PID_KI              1.06    // Default Integral Gain
#define PID_KD              116.55  // Default Derivative Gain


// --- Safety & Peripherals ---
#define SAFETY_RELAY_PIN    10      // E-Stop Relay (cuts all power)
#define FAN_PIN             4       // Cooling fan for electronics
#define BUZZER_PIN          5       // Audible feedback


// --- General Config ---
#define FIRMWARE_VERSION    "1.2.0"
#define DEVICE_NAME         "Trae Kiln Controller"
