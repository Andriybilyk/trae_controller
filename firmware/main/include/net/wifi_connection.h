#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
#include <string>
extern "C" {
#endif

/**
 * @brief Returns true when a real Wi-Fi backend is compiled in.
 * On NewP4 builds without C6/hosted integration this returns false.
 */
bool wifi_backend_available(void);

/**
 * @brief Human-readable Wi-Fi backend name for logs/diagnostics.
 */
const char* wifi_backend_name(void);

/**
 * @brief Notify WiFi layer about firing state to reduce interference.
 */
void wifi_set_firing_active(bool active);

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
 * @brief Start an asynchronous WiFi scan.
 */
void wifi_start_scan(void);

/**
 * @brief Get the status of the WiFi scan.
 */
bool wifi_is_scan_done(void);
bool wifi_scan_in_progress(void);

void wifi_get_scanned_networks_c(char *out, int32_t out_len);

/**
 * @brief Check current STA link status.
 */
bool wifi_is_connected(void);
uint64_t wifi_last_sta_got_ip_ms(void);
bool wifi_sta_connect_in_progress(void);

/**
 * @brief User-facing Wi-Fi enable flag stored in NVS.
 * When disabled, STA won't auto-connect; controller should stay in SoftAP for setup.
 */
bool wifi_is_user_enabled(void);
void wifi_set_user_enabled(bool enabled);

/**
 * @brief Disconnect STA without erasing saved credentials.
 */
void wifi_disconnect_sta(void);

/**
 * @brief Force Wi-Fi driver into STA-only mode (disable AP interface) when STA connected.
 */
void wifi_set_sta_only_mode(void);

/**
 * @brief Check whether AP interface is currently enabled (AP/APSTA mode).
 */
bool wifi_ap_active(void);

/**
 * @brief C-friendly helpers for UI (avoid std::string in C files).
 */
void wifi_get_server_url_c(char *out, int32_t out_len);
void wifi_get_ap_ssid_c(char *out, int32_t out_len);
void wifi_get_sta_ssid_c(char *out, int32_t out_len);

/**
 * @brief Save credentials for manual connect flow.
 */
bool wifi_connect_with_credentials(const char* ssid, const char* password);

/**
 * @brief Disconnect current STA and erase saved credentials.
 */
void wifi_disconnect_and_forget(void);

/**
 * @brief Start ESP-IDF Wi-Fi provisioning (SoftAP + protocomm).
 */
bool wifi_prov_start_softap(void);

/**
 * @brief Stop active provisioning session.
 */
void wifi_prov_stop(void);

/**
 * @brief Returns provisioning runtime state.
 */
bool wifi_prov_is_active(void);

#ifdef __cplusplus
}

std::string wifi_get_scanned_networks(void);
std::string wifi_get_server_url(void);
std::string wifi_get_ap_ssid(void);
std::string wifi_prov_service_name(void);
std::string wifi_prov_pop(void);
#endif
