#pragma once

#include "esp_err.h"
#include <string>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi in Station mode.
 * @return true if connected, false otherwise.
 */
bool wifi_init_sta(void);

/**
 * @brief Initialize WiFi in SoftAP mode (for configuration).
 */
void wifi_init_softap(void);

/**
 * @brief Check if WiFi credentials are saved in NVS.
 */
bool wifi_is_configured(void);

/**
 * @brief Save WiFi credentials to NVS.
 */
void wifi_save_creds(const char* ssid, const char* password);

/**
 * @brief Erase WiFi credentials from NVS.
 */
void wifi_erase_creds(void);

/**
 * @brief Scan for available networks and return JSON string.
 */
std::string wifi_get_scanned_networks(void);

/**
 * @brief Start an asynchronous WiFi scan.
 */
void wifi_start_scan(void);

/**
 * @brief Get the status of the WiFi scan.
 */
bool wifi_is_scan_done(void);

/**
 * @brief Check current STA link status.
 */
bool wifi_is_connected(void);

/**
 * @brief Save credentials for manual connect flow.
 */
bool wifi_connect_with_credentials(const char* ssid, const char* password);

/**
 * @brief Disconnect current STA and erase saved credentials.
 */
void wifi_disconnect_and_forget(void);

#ifdef __cplusplus
}
#endif
