#include "net/wifi_connection.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "cJSON.h"
#include "kiln_config/config.h"
#include "drivers/rtc_ds3231.h"
#include "config/board_profile.h"
#include "net/dns_server.h"

#include <time.h>
#include <stdlib.h>

#if __has_include("wifi_provisioning/manager.h")
#define WIFI_PROV_MGR_AVAILABLE 1
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"
#else
#define WIFI_PROV_MGR_AVAILABLE 0
#endif

static const char *TAG = "WIFI_CONN";
static constexpr const char *kCaptivePortalUrl = "http://192.168.4.1/captive-portal/api";
static constexpr const char *kControllerApSsidPrefix = "TRAE_KILN_SETUP";
static char s_controller_ap_ssid[33] = "TRAE_KILN_SETUP";
// Temporary debug switch: disable STA connection attempts to isolate display flicker from Wi-Fi state transitions.
static constexpr bool kDebugDisableStaConnect = false;

static bool read_any_mac_for_ap_suffix(uint8_t out_mac[6]) {
    if (!out_mac) return false;
    memset(out_mac, 0, 6);

    // Preferred source for AP identity.
    if (esp_read_mac(out_mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) return true;

    // Fallbacks for targets where SOFTAP MAC type is not exposed by hosted layer.
    if (esp_read_mac(out_mac, ESP_MAC_WIFI_STA) == ESP_OK) return true;
#ifdef ESP_MAC_EFUSE_FACTORY
    if (esp_read_mac(out_mac, ESP_MAC_EFUSE_FACTORY) == ESP_OK) return true;
#endif
    return false;
}

static void ensure_controller_ap_ssid(void) {
    if (strlen(s_controller_ap_ssid) > strlen(kControllerApSsidPrefix)) {
        return;
    }
    uint8_t mac[6] = {0};
    if (!read_any_mac_for_ap_suffix(mac)) {
        const uint32_t rnd = esp_random() & 0xFFFFU;
        std::snprintf(
            s_controller_ap_ssid,
            sizeof(s_controller_ap_ssid),
            "%s_%04X",
            kControllerApSsidPrefix,
            (unsigned)rnd
        );
        ESP_LOGW(TAG, "AP SSID suffix: MAC unavailable, using random fallback '%s'", s_controller_ap_ssid);
        return;
    }
    std::snprintf(
        s_controller_ap_ssid,
        sizeof(s_controller_ap_ssid),
        "%s_%02X%02X",
        kControllerApSsidPrefix,
        (unsigned)mac[4],
        (unsigned)mac[5]
    );
    ESP_LOGI(TAG, "AP SSID resolved: %s", s_controller_ap_ssid);
}

#if !defined(CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM) && !defined(CONFIG_ESP_HOSTED_ENABLED)

static bool s_stub_scan_done = false;
static std::string s_stub_scan_result = "[]";

bool wifi_backend_available(void) {
    return false;
}

const char* wifi_backend_name(void) {
    return "none";
}

void wifi_start_scan(void) {
    s_stub_scan_result = "[]";
    s_stub_scan_done = true;
    ESP_LOGW(TAG, "Wi-Fi scan requested, but Wi-Fi stack is unavailable for this build/profile");
}

bool wifi_is_scan_done(void) {
    const bool done = s_stub_scan_done;
    s_stub_scan_done = false;
    return done;
}

std::string wifi_get_scanned_networks(void) {
    return s_stub_scan_result;
}

void wifi_save_creds(const char* ssid, const char* password) {
    (void)ssid;
    (void)password;
}

void wifi_erase_creds() {}

bool wifi_is_configured() {
    return false;
}

bool wifi_init_sta(void) {
    ESP_LOGW(TAG, "Wi-Fi stack is not available on this target profile yet");
    return false;
}

void wifi_init_softap(void) {
    ESP_LOGW(TAG, "SoftAP is not available on this target profile yet");
}

std::string wifi_scan_networks(void) {
    return "[]";
}

bool wifi_is_connected(void) {
    return false;
}

bool wifi_ap_active(void) {
    return false;
}

std::string wifi_get_server_url(void) {
    return std::string("http://192.168.4.1");
}

std::string wifi_get_ap_ssid(void) {
    return std::string(kControllerApSsidPrefix);
}

bool wifi_connect_with_credentials(const char* ssid, const char* password) {
    (void)ssid;
    (void)password;
    ESP_LOGW(TAG, "Wi-Fi connect requested, but Wi-Fi stack is unavailable for this build/profile");
    return false;
}

void wifi_disconnect_and_forget(void) {}

bool wifi_prov_start_softap(void) {
    ESP_LOGW(TAG, "Provisioning manager is unavailable on this target profile");
    return false;
}

void wifi_prov_stop(void) {}

bool wifi_prov_is_active(void) {
    return false;
}

std::string wifi_prov_service_name(void) {
    return std::string("TRAE_PROV");
}

std::string wifi_prov_pop(void) {
    return std::string("");
}

#else

// --- NVS Storage ---
#define NVS_NAMESPACE "wifi_config"
#define KEY_SSID      "ssid"
#define KEY_PASS      "password"

// --- Scan Task ---
static TaskHandle_t wifi_scan_task_handle = NULL;
static SemaphoreHandle_t scan_done_sem = NULL;
static SemaphoreHandle_t s_connect_mutex = NULL;
static std::string s_last_good_scan_result = "[]";
static std::string scan_result_json;
static bool s_scan_in_progress = false;
static bool s_sntp_started = false;
static bool s_scan_done_event = false;
static int s_scan_done_status = -1;
static bool s_scan_done_handler_registered = false;
static esp_event_handler_instance_t s_scan_done_instance = nullptr;
static constexpr uint32_t WIFI_SCAN_TASK_STACK = 8192;
static bool s_prov_active = false;
static bool s_prov_initialized = false;
static char s_prov_service_name[16] = "TRAE_PROV";
static char s_prov_pop[16] = "12345678";
static bool s_prov_handler_registered = false;
static char s_captive_portal_uri[96] = "http://192.168.4.1/captive-portal/api";
static bool s_wifi_driver_ready = false;
static bool s_ap_event_handler_registered = false;
static SemaphoreHandle_t s_wifi_init_mutex = NULL;

static bool wifi_init_lock_take(TickType_t timeout_ticks = pdMS_TO_TICKS(4000)) {
    if (s_wifi_init_mutex == NULL) {
        s_wifi_init_mutex = xSemaphoreCreateMutex();
        if (s_wifi_init_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create wifi init mutex");
            return false;
        }
    }
    if (xSemaphoreTake(s_wifi_init_mutex, timeout_ticks) != pdTRUE) {
        ESP_LOGW(TAG, "wifi init lock timeout");
        return false;
    }
    return true;
}

static void wifi_init_lock_give() {
    if (s_wifi_init_mutex) {
        (void)xSemaphoreGive(s_wifi_init_mutex);
    }
}

static void ensure_ap_netif_ip_192_168_4_1() {
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) {
        ESP_LOGW(TAG, "WIFI_AP_DEF is null; cannot enforce AP IP");
        return;
    }

    esp_netif_ip_info_t ip_info = {};
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    (void)esp_netif_dhcps_stop(ap);
    const esp_err_t ip_err = esp_netif_set_ip_info(ap, &ip_info);
    if (ip_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_set_ip_info(AP) failed: %s", esp_err_to_name(ip_err));
    }
    const esp_err_t dhcp_err = esp_netif_dhcps_start(ap);
    if (dhcp_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_dhcps_start(AP) failed: %s", esp_err_to_name(dhcp_err));
    } else {
        ESP_LOGI(TAG, "AP netif enforced: 192.168.4.1/24");
    }

    // iOS captive assistant depends on DHCP-provided DNS for this network.
    // Force DNS server to AP IP and explicitly offer DNS/router options via DHCP.
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip_info.ip.addr;
    const esp_err_t dns_set_err = esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    if (dns_set_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_set_dns_info(AP) failed: %s", esp_err_to_name(dns_set_err));
    }

    uint8_t offer_dns = 1;
    const esp_err_t dns_offer_err = esp_netif_dhcps_option(
        ap,
        ESP_NETIF_OP_SET,
        ESP_NETIF_DOMAIN_NAME_SERVER,
        &offer_dns,
        sizeof(offer_dns)
    );
    if (dns_offer_err != ESP_OK) {
        ESP_LOGW(TAG, "dhcps DOMAIN_NAME_SERVER offer failed: %s", esp_err_to_name(dns_offer_err));
    }

    // Keep DHCP options minimal/standard for better iOS compatibility.
}

static const char* wifi_mode_to_str(wifi_mode_t mode) {
    switch (mode) {
        case WIFI_MODE_NULL: return "NULL";
        case WIFI_MODE_STA: return "STA";
        case WIFI_MODE_AP: return "AP";
        case WIFI_MODE_APSTA: return "APSTA";
        default: return "UNKNOWN";
    }
}

bool wifi_backend_available(void) {
    return true;
}

const char* wifi_backend_name(void) {
    return "esp_wifi";
}

#if WIFI_PROV_MGR_AVAILABLE
static void wifi_prov_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data) {
    (void)arg;
    (void)event_data;
    if (event_base != WIFI_PROV_EVENT) return;
    switch (event_id) {
        case WIFI_PROV_START:
            s_prov_active = true;
            ESP_LOGI(TAG, "Wi-Fi provisioning started");
            break;
        case WIFI_PROV_CRED_RECV:
            ESP_LOGI(TAG, "Wi-Fi provisioning credentials received");
            break;
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Wi-Fi provisioning credentials accepted");
            break;
        case WIFI_PROV_CRED_FAIL:
            ESP_LOGW(TAG, "Wi-Fi provisioning credentials rejected");
            break;
        case WIFI_PROV_END:
            ESP_LOGI(TAG, "Wi-Fi provisioning ended");
            s_prov_active = false;
            if (s_prov_initialized) {
                wifi_prov_mgr_deinit();
                s_prov_initialized = false;
            }
            break;
        default:
            break;
    }
}
#endif

static void sntp_time_sync_cb(struct timeval *) {
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        return;
    }
    (void)rtc_ds3231_sync_from_system_time();
}

static void sntp_start_if_needed(void) {
    if (s_sntp_started) return;
    s_sntp_started = true;

    setenv("TZ", TIMEZONE_TZ, 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, SNTP_SERVER_1);
    sntp_set_time_sync_notification_cb(sntp_time_sync_cb);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (server=%s, TZ=%s)", SNTP_SERVER_1, TIMEZONE_TZ);
}

static void wifi_scan_done_event_handler(void* arg, esp_event_base_t event_base,
                                         int32_t event_id, void* event_data) {
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        wifi_event_sta_scan_done_t *ev = reinterpret_cast<wifi_event_sta_scan_done_t*>(event_data);
        s_scan_done_event = true;
        s_scan_done_status = ev ? (int)ev->status : 0;
        ESP_LOGI(TAG, "WIFI_EVENT_SCAN_DONE status=%d", s_scan_done_status);
    }
}

static void wifi_scan_task(void *pvParameters) {
    ESP_LOGI(TAG, "Starting WiFi Scan Task (stack=%u, free_heap=%u, free_internal=%u)...",
             (unsigned)WIFI_SCAN_TASK_STACK,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    s_scan_in_progress = true;
    
    wifi_scan_config_t scan_config = {};
    scan_config.ssid = NULL;
    scan_config.bssid = NULL;
    scan_config.channel = 0;
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 60;
    scan_config.scan_time.active.max = 240;

    s_scan_done_event = false;
    s_scan_done_status = -1;
    const esp_err_t scan_start = esp_wifi_scan_start(&scan_config, false);
    if (scan_start == ESP_OK) {
        constexpr TickType_t kScanTimeout = pdMS_TO_TICKS(8000);
        const TickType_t start_ticks = xTaskGetTickCount();
        while (!s_scan_done_event && (xTaskGetTickCount() - start_ticks) < kScanTimeout) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!s_scan_done_event) {
            ESP_LOGE(TAG, "WiFi scan timeout (8s), stopping scan");
            (void)esp_wifi_scan_stop();
            scan_result_json = s_last_good_scan_result;
            s_scan_in_progress = false;
            wifi_scan_task_handle = NULL;
            if (scan_done_sem) {
                xSemaphoreGive(scan_done_sem);
            }
            vTaskDelete(NULL);
            return;
        }
        uint16_t ap_num = 0;
        const esp_err_t ap_num_ret = esp_wifi_scan_get_ap_num(&ap_num);
        if (ap_num_ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(ap_num_ret));
            ap_num = 0;
        }
        ESP_LOGI(TAG, "WiFi scan done, AP count=%u", (unsigned)ap_num);
        if (ap_num > 0) {
            wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(ap_num * sizeof(wifi_ap_record_t));
            if (!ap_list) {
                ESP_LOGE(TAG, "Failed to allocate AP list buffer for %u records", (unsigned)ap_num);
            }
            const esp_err_t rec_ret = ap_list ? esp_wifi_scan_get_ap_records(&ap_num, ap_list) : ESP_ERR_NO_MEM;
            if (ap_list && rec_ret == ESP_OK) {
                cJSON *root = cJSON_CreateArray();
                int added = 0;
                for (int i = 0; i < ap_num; i++) {
                    if (strlen((char *)ap_list[i].ssid) == 0) continue;

                    cJSON *obj = cJSON_CreateObject();
                    cJSON_AddStringToObject(obj, "ssid", (char *)ap_list[i].ssid);
                    cJSON_AddNumberToObject(obj, "rssi", ap_list[i].rssi);
                    cJSON_AddNumberToObject(obj, "auth", ap_list[i].authmode);
                    cJSON_AddItemToArray(root, obj);
                    added++;
                }

                char *rendered = cJSON_PrintUnformatted(root);
                scan_result_json.assign(rendered ? rendered : "[]");
                if (added > 0) {
                    s_last_good_scan_result = scan_result_json;
                } else if (s_last_good_scan_result != "[]") {
                    scan_result_json = s_last_good_scan_result;
                }
                if (rendered) free(rendered);
                cJSON_Delete(root);
                ESP_LOGI(TAG, "WiFi scan JSON size=%u bytes", (unsigned)scan_result_json.size());
            } else if (rec_ret != ESP_OK) {
                ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(rec_ret));
                scan_result_json = s_last_good_scan_result;
            }
            free(ap_list);
        } else {
            scan_result_json = s_last_good_scan_result;
        }
    } else {
        scan_result_json = s_last_good_scan_result;
        ESP_LOGE(TAG, "WiFi scan failed to start: %s", esp_err_to_name(scan_start));
    }
    
    ESP_LOGI(TAG, "WiFi Scan completed (stack_watermark=%u words)", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_scan_in_progress = false;
    wifi_scan_task_handle = NULL;
    if (scan_done_sem) {
        xSemaphoreGive(scan_done_sem);
    } else {
        ESP_LOGE(TAG, "scan_done_sem is NULL at scan completion");
    }
    vTaskDelete(NULL); // Delete self
}

void wifi_start_scan(void) {
    if (scan_done_sem == NULL) {
        scan_done_sem = xSemaphoreCreateBinary();
        if (scan_done_sem == NULL) {
            ESP_LOGE(TAG, "Failed to create scan_done_sem");
            return;
        }
    }
    // Take semaphore before starting, so we can wait for it
    xSemaphoreTake(scan_done_sem, 0); 
    
    // Ensure APSTA mode for scanning
    wifi_mode_t mode = WIFI_MODE_NULL;
    const esp_err_t mode_ret = esp_wifi_get_mode(&mode);
    if (mode_ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_mode failed before scan: %s", esp_err_to_name(mode_ret));
    } else {
        ESP_LOGI(TAG, "wifi_start_scan backend=%s mode=%s free_heap=%u free_internal=%u",
                 wifi_backend_name(), wifi_mode_to_str(mode),
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    if (mode == WIFI_MODE_AP) {
        const esp_err_t set_mode_ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (set_mode_ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(set_mode_ret));
            scan_result_json = s_last_good_scan_result;
            if (scan_done_sem) xSemaphoreGive(scan_done_sem);
            return;
        }
        ESP_LOGI(TAG, "WiFi mode switched AP -> APSTA for scan");
    }
    
    if (!s_scan_done_handler_registered) {
        const esp_err_t reg_ret = esp_event_handler_instance_register(
            WIFI_EVENT,
            WIFI_EVENT_SCAN_DONE,
            &wifi_scan_done_event_handler,
            nullptr,
            &s_scan_done_instance
        );
        if (reg_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register WIFI_EVENT_SCAN_DONE handler: %s", esp_err_to_name(reg_ret));
            scan_result_json = s_last_good_scan_result;
            if (scan_done_sem) xSemaphoreGive(scan_done_sem);
            return;
        }
        s_scan_done_handler_registered = true;
    }

    if (s_scan_in_progress || wifi_scan_task_handle != NULL) {
        ESP_LOGW(TAG, "WiFi scan already in progress");
        if (scan_done_sem) xSemaphoreGive(scan_done_sem);
        return;
    }

    const BaseType_t task_ok = xTaskCreate(wifi_scan_task, "wifi_scan", WIFI_SCAN_TASK_STACK, NULL, 5, &wifi_scan_task_handle);
    if (task_ok != pdPASS) {
        wifi_scan_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create wifi_scan task");
        scan_result_json = s_last_good_scan_result;
        if (scan_done_sem) xSemaphoreGive(scan_done_sem);
        return;
    }
    ESP_LOGI(TAG, "WiFi scan task created");
}

bool wifi_is_scan_done(void) {
    if (scan_done_sem == NULL) return false;
    const bool done = xSemaphoreTake(scan_done_sem, 0) == pdTRUE;
    if (done) {
        ESP_LOGI(TAG, "wifi_is_scan_done=true (result_size=%u)", (unsigned)scan_result_json.size());
    }
    return done;
}

std::string wifi_get_scanned_networks(void) {
    return scan_result_json;
}


void wifi_save_creds(const char* ssid, const char* password) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return;
    }

    nvs_set_str(my_handle, KEY_SSID, ssid);
    nvs_set_str(my_handle, KEY_PASS, password);
    nvs_commit(my_handle);
    nvs_close(my_handle);
    ESP_LOGI(TAG, "WiFi credentials saved.");
}

void wifi_erase_creds() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_erase_all(my_handle);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "WiFi credentials erased.");
    }
}

bool wifi_is_configured() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) return false;

    size_t ssid_len = 0;
    nvs_get_str(my_handle, KEY_SSID, NULL, &ssid_len);
    nvs_close(my_handle);
    
    return (err == ESP_OK && ssid_len > 1); // Check if ssid is not empty
}

// --- STA Mode ---
#define MAXIMUM_RETRY  5
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;
static volatile uint64_t s_last_sta_got_ip_ms = 0;
static bool s_firing_active = false;

void wifi_set_firing_active(bool active) {
    if (s_firing_active != active) {
        ESP_LOGI(TAG, "WiFi firing mode: %s", active ? "ACTIVE (reduced interference)" : "INACTIVE");
        s_firing_active = active;
    }
}

static esp_event_handler_instance_t s_sta_instance_any_id = nullptr;
static esp_event_handler_instance_t s_sta_instance_got_ip = nullptr;
static void ap_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void configure_ap_captive_portal_option();

static void sta_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA_START: connect begin");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "STA_DISCONNECTED: reason=%d retry=%d", ev ? (int)ev->reason : -1, s_retry_num);
        if (s_retry_num < MAXIMUM_RETRY) {
            // If firing is active, wait longer before retrying to avoid display flicker
            // caused by SDIO/Bus contention during high-power EMI events.
            if (s_firing_active) {
                ESP_LOGW(TAG, "STA_DISCONNECTED during firing, delaying retry by 5s...");
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        s_last_sta_got_ip_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "STA_GOT_IP: Wi-Fi connected");
        s_retry_num = 0;
        if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        sntp_start_if_needed();
    }
}

uint64_t wifi_last_sta_got_ip_ms(void) {
    return s_last_sta_got_ip_ms;
}

bool wifi_init_sta(void) {
    if (kDebugDisableStaConnect) {
        ESP_LOGW(TAG, "DEBUG: STA connect is disabled (kDebugDisableStaConnect=true)");
        return false;
    }
    if (!wifi_init_lock_take()) {
        return false;
    }
    nvs_handle_t my_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle) != ESP_OK) {
        wifi_init_lock_give();
        return false;
    }

    char ssid[33] = {0};
    char pass[65] = {0};
    size_t len = sizeof(ssid);
    nvs_get_str(my_handle, KEY_SSID, ssid, &len);
    len = sizeof(pass);
    nvs_get_str(my_handle, KEY_PASS, pass, &len);
    nvs_close(my_handle);

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    ensure_controller_ap_ssid();
    if (strcmp(ssid, s_controller_ap_ssid) == 0 ||
        strncmp(ssid, kControllerApSsidPrefix, strlen(kControllerApSsidPrefix)) == 0) {
        ESP_LOGW(TAG, "Stored STA SSID points to controller AP (%s). Clearing invalid creds.", ssid);
        wifi_erase_creds();
        wifi_init_lock_give();
        return false;
    }

    s_wifi_event_group = xEventGroupCreate();

    {
        const esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
            wifi_init_lock_give();
            return false;
        }
    }
    {
        const esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
            wifi_init_lock_give();
            return false;
        }
    }
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == nullptr) {
        esp_netif_create_default_wifi_sta();
    }
    // AP netif is required in APSTA mode so phones connected to TRAE_KILN_SETUP
    // receive 192.168.4.x (DHCP) and can open captive/web pages.
    if (esp_netif_get_handle_from_ifkey("WIFI_AP_DEF") == nullptr) {
        esp_netif_create_default_wifi_ap();
    }

    if (!s_wifi_driver_ready) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        const esp_err_t init_err = esp_wifi_init(&cfg);
        if (init_err != ESP_OK && init_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(init_err));
            wifi_init_lock_give();
            return false;
        }
        s_wifi_driver_ready = true;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, NULL, &s_sta_instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, NULL, &s_sta_instance_got_ip);
    if (!s_ap_event_handler_registered) {
        const esp_err_t reg_err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &ap_event_handler, NULL, NULL);
        if (reg_err == ESP_OK || reg_err == ESP_ERR_INVALID_STATE) {
            s_ap_event_handler_registered = true;
        }
    }

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, ssid);
    strcpy((char*)wifi_config.sta.password, pass);

    wifi_config_t ap_config = {};
    const char *ap_ssid = s_controller_ap_ssid;
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.password[0] = '\0';
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.ssid_hidden = 0;
    ap_config.ap.max_connection = 4;
    ap_config.ap.pmf_cfg.required = false;

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    ensure_ap_netif_ip_192_168_4_1();
    configure_ap_captive_portal_option();

    // Use a reasonable timeout (10s) instead of portMAX_DELAY to avoid blocking the server task indefinitely,
    // which can interfere with the UI startup on NewP4.
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));

    if (s_sta_instance_any_id) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_sta_instance_any_id);
        s_sta_instance_any_id = nullptr;
    }
    if (s_sta_instance_got_ip) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_sta_instance_got_ip);
        s_sta_instance_got_ip = nullptr;
    }
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = nullptr;

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", ssid);
        wifi_init_lock_give();
        return true;
    } else {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", ssid);
        esp_wifi_stop();
        wifi_init_lock_give();
        return false;
    }
}

// --- AP Mode ---
static void ap_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    (void)arg;
    (void)event_base;
    if (event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "SoftAP started -> starting DNS captive responder");
        start_dns_server();
    } else if (event_id == WIFI_EVENT_AP_STOP) {
        ESP_LOGI(TAG, "SoftAP stopped -> stopping DNS captive responder");
        stop_dns_server();
    } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGW(TAG, "station " MACSTR " leave, AID=%d, reason=%d",
                 MAC2STR(event->mac), event->aid, (int)event->reason);
    }
}

void wifi_init_softap(void)
{
    if (!wifi_init_lock_take()) {
        return;
    }
    ensure_controller_ap_ssid();
    {
        const esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
            wifi_init_lock_give();
            return;
        }
    }
    {
        const esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
            wifi_init_lock_give();
            return;
        }
    }
    if (esp_netif_get_handle_from_ifkey("WIFI_AP_DEF") == nullptr) {
        esp_netif_create_default_wifi_ap();
    }

    if (!s_wifi_driver_ready) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        const esp_err_t init_err = esp_wifi_init(&cfg);
        if (init_err != ESP_OK && init_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(init_err));
            wifi_init_lock_give();
            return;
        }
        s_wifi_driver_ready = true;
    }

    if (!s_ap_event_handler_registered) {
        const esp_err_t reg_err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &ap_event_handler, NULL, NULL);
        if (reg_err == ESP_OK || reg_err == ESP_ERR_INVALID_STATE) {
            s_ap_event_handler_registered = true;
        }
    }

    wifi_config_t wifi_config = {};
    const char *ap_ssid = s_controller_ap_ssid;
    strncpy((char *)wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(ap_ssid);
    wifi_config.ap.channel = 1;
    wifi_config.ap.password[0] = '\0';
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    wifi_config.ap.ssid_hidden = 0;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.pmf_cfg.required = false;

    // Setup portal should be a clean SoftAP for better client compatibility (especially iOS).
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();
    ensure_ap_netif_ip_192_168_4_1();
    configure_ap_captive_portal_option();

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s", s_controller_ap_ssid);
    wifi_init_lock_give();
}

static void configure_ap_captive_portal_option() {
    // iOS auto-popup behavior can be less reliable when CAPPORT DHCP option is present.
    // Keep legacy captive detection path (DNS hijack + probe URLs) only.
    ESP_LOGI(TAG, "CAPPORT DHCP option disabled (legacy captive detection mode)");
    return;

    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) {
        ESP_LOGW(TAG, "Cannot set captive portal DHCP option: WIFI_AP_DEF netif is null");
        return;
    }
    std::snprintf(s_captive_portal_uri, sizeof(s_captive_portal_uri), "%s", kCaptivePortalUrl);
    esp_err_t opt_err = esp_netif_dhcps_option(
        ap,
        ESP_NETIF_OP_SET,
        ESP_NETIF_CAPTIVEPORTAL_URI,
        s_captive_portal_uri,
        (uint32_t)(strlen(s_captive_portal_uri) + 1)
    );
    if (opt_err != ESP_OK) {
        ESP_LOGW(TAG, "CAPPORT URI set failed: %s", esp_err_to_name(opt_err));
    } else {
        ESP_LOGI(TAG, "CAPPORT URI set: %s", s_captive_portal_uri);
    }
}

// Kept for compatibility, but it's now a blocking wrapper. Avoid using in handlers.
std::string wifi_scan_networks(void) {
    wifi_start_scan();
    if(scan_done_sem) xSemaphoreTake(scan_done_sem, portMAX_DELAY); // Block until done
    return wifi_get_scanned_networks();
}

bool wifi_is_connected(void) {
    bool has_sta_ip = false;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_ip_info_t ip_info = {};
        if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            has_sta_ip = true;
        }
    }
    // On some ESP-Hosted combinations, esp_wifi_sta_get_ap_info() can be transiently
    // unavailable even after station gets a valid DHCP lease. For portal UX we treat
    // acquired STA IP as connected state.
    return has_sta_ip;
}

bool wifi_ap_active(void) {
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK) return false;
    return mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA;
}

std::string wifi_get_server_url(void) {
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_ip_info_t ip_info = {};
        if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            char url[32];
            snprintf(url, sizeof(url), "http://" IPSTR, IP2STR(&ip_info.ip));
            return std::string(url);
        }
    }
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap) {
        esp_netif_ip_info_t ip_info = {};
        if (esp_netif_get_ip_info(ap, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            char url[32];
            snprintf(url, sizeof(url), "http://" IPSTR, IP2STR(&ip_info.ip));
            return std::string(url);
        }
    }
    return std::string("http://192.168.4.1");
}

std::string wifi_get_ap_ssid(void) {
    ensure_controller_ap_ssid();
    return std::string(s_controller_ap_ssid);
}

bool wifi_connect_with_credentials(const char* ssid, const char* password) {
    if (!ssid || !ssid[0]) return false;
    if (s_connect_mutex == NULL) {
        s_connect_mutex = xSemaphoreCreateMutex();
        if (s_connect_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create connect mutex");
            return false;
        }
    }
    if (xSemaphoreTake(s_connect_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "Rejecting connect request: previous connection attempt still in progress");
        return false;
    }
    bool ok = false;
    do {
        ensure_controller_ap_ssid();
        if (strcmp(ssid, s_controller_ap_ssid) == 0 ||
            strncmp(ssid, kControllerApSsidPrefix, strlen(kControllerApSsidPrefix)) == 0) {
            ESP_LOGW(TAG, "Rejecting connect to controller AP SSID as STA uplink: %s", ssid);
            break;
        }
        const char *pass = password ? password : "";
        wifi_save_creds(ssid, pass);

        wifi_mode_t mode = WIFI_MODE_NULL;
        const esp_err_t mode_err = esp_wifi_get_mode(&mode);
        if (mode_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_get_mode failed before connect: %s", esp_err_to_name(mode_err));
        }
        if (mode == WIFI_MODE_NULL || mode == WIFI_MODE_AP) {
            const esp_err_t set_mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
            if (set_mode_err != ESP_OK) {
                ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed for SSID='%s': %s", ssid, esp_err_to_name(set_mode_err));
                break;
            }
        }

        // Apply credentials immediately so UI gets instant connection attempt after password confirmation.
        wifi_config_t wifi_config = {};
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

        const esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (cfg_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_config failed for SSID='%s': %s", ssid, esp_err_to_name(cfg_err));
            break;
        }

        (void)esp_wifi_disconnect();
        const esp_err_t conn_err = esp_wifi_connect();
        if (conn_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect failed for SSID='%s': %s", ssid, esp_err_to_name(conn_err));
            break;
        }

        ESP_LOGI(TAG, "Wi-Fi reconnect requested for SSID='%s'", ssid);
        ok = true;
    } while (false);
    (void)xSemaphoreGive(s_connect_mutex);
    return ok;
}

void wifi_disconnect_and_forget(void) {
    (void)esp_wifi_disconnect();
    wifi_erase_creds();
}

bool wifi_prov_start_softap(void) {
#if !WIFI_PROV_MGR_AVAILABLE
    ESP_LOGW(TAG, "wifi_prov_mgr is unavailable in this ESP-IDF build");
    return false;
#else
    if (s_prov_active) return true;

    uint8_t eth_mac[6] = {0};
    if (esp_read_mac(eth_mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) {
        std::snprintf(s_prov_service_name, sizeof(s_prov_service_name),
                      "TRAE_%02X%02X%02X", eth_mac[3], eth_mac[4], eth_mac[5]);
    }

    if (!s_prov_handler_registered) {
        const esp_err_t reg_err = esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &wifi_prov_event_handler, nullptr);
        if (reg_err != ESP_OK && reg_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "WIFI_PROV_EVENT register failed: %s", esp_err_to_name(reg_err));
            return false;
        }
        s_prov_handler_registered = true;
    }

    wifi_prov_mgr_config_t config = {};
    config.scheme = wifi_prov_scheme_softap;
    config.scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE;
    esp_err_t err = wifi_prov_mgr_init(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "wifi_prov_mgr_init failed: %s", esp_err_to_name(err));
        return false;
    }
    s_prov_initialized = true;

    bool provisioned = false;
    err = wifi_prov_mgr_is_provisioned(&provisioned);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_is_provisioned failed: %s", esp_err_to_name(err));
        return false;
    }
    if (provisioned) {
        ESP_LOGI(TAG, "Provisioning skipped: already provisioned");
        return false;
    }

    err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1,
                                           reinterpret_cast<const void*>(s_prov_pop),
                                           s_prov_service_name,
                                           nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_start_provisioning failed: %s", esp_err_to_name(err));
        return false;
    }
    s_prov_active = true;
    ESP_LOGI(TAG, "Provisioning active (service=%s, pop=%s)", s_prov_service_name, s_prov_pop);
    return true;
#endif
}

void wifi_prov_stop(void) {
#if WIFI_PROV_MGR_AVAILABLE
    if (!s_prov_initialized) {
        s_prov_active = false;
        return;
    }
    (void)wifi_prov_mgr_stop_provisioning();
    wifi_prov_mgr_deinit();
    s_prov_initialized = false;
    s_prov_active = false;
#endif
}

bool wifi_prov_is_active(void) {
    return s_prov_active;
}

std::string wifi_prov_service_name(void) {
    return std::string(s_prov_service_name);
}

std::string wifi_prov_pop(void) {
    return std::string(s_prov_pop);
}

#endif
