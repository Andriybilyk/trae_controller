#pragma once

#include "esp_err.h"
#include <string>

#ifdef __cplusplus
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
uint64_t wifi_last_sta_got_ip_ms(void);

/**
 * @brief Check whether AP interface is currently enabled (AP/APSTA mode).
 */
bool wifi_ap_active(void);

/**
 * @brief Get URL for web interface.
 * Returns STA URL when connected, otherwise SoftAP fallback.
 */
std::string wifi_get_server_url(void);

/**
 * @brief Get active controller SoftAP SSID.
 */
std::string wifi_get_ap_ssid(void);

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

/**
 * @brief Service name exposed by provisioning SoftAP.
 */
std::string wifi_prov_service_name(void);

/**
 * @brief Proof-of-possession string used by provisioning security.
 */
std::string wifi_prov_pop(void);

#ifdef __cplusplus
}
#endif
