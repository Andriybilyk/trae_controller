#include "wifi_connection.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "ArduinoJson.h"

static const char *TAG = "WIFI_CONN";

// --- NVS Storage ---
#define NVS_NAMESPACE "wifi_config"
#define KEY_SSID      "ssid"
#define KEY_PASS      "password"

void wifi_save_creds(const char* ssid, const char* password) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(my_handle, KEY_SSID, ssid);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to save SSID");

    err = nvs_set_str(my_handle, KEY_PASS, password);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to save Password");

    err = nvs_commit(my_handle);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to commit NVS");

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
    err = nvs_get_str(my_handle, KEY_SSID, NULL, &ssid_len);
    nvs_close(my_handle);
    
    return (err == ESP_OK && ssid_len > 0);
}

// --- STA Mode ---
#define MAXIMUM_RETRY  5
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

static void sta_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

bool wifi_init_sta(void) {
    // Load credentials
    nvs_handle_t my_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle) != ESP_OK) return false;

    char ssid[33] = {0};
    char pass[65] = {0};
    size_t len = sizeof(ssid);
    nvs_get_str(my_handle, KEY_SSID, ssid, &len);
    len = sizeof(pass);
    nvs_get_str(my_handle, KEY_PASS, pass, &len);
    nvs_close(my_handle);

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    s_wifi_event_group = xEventGroupCreate();

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }
    
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }
    
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &sta_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &sta_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, ssid);
    strcpy((char*)wifi_config.sta.password, pass);
    wifi_config.sta.threshold.rssi = -127;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", ssid);
        return true;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", ssid);
        // Clean up to allow retry or mode switch? 
        // For now just return false, caller should handle fallback
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }
    return false;
}

// --- AP Mode ---
static void ap_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

void wifi_init_softap(void)
{
    // If we are here, we might need to re-init netif if STA failed and de-inited it.
    // Ideally we should check if already inited.
    // Simplified: Assume we call this if STA failed or fresh start.
    
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }
    
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }
    
    // Create AP if not exists. 
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    
    // Config AP IP to 192.168.4.1 (default)

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // Check if wifi is already inited?
    // If we came from failed STA, we de-inited it.
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &ap_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "TRAE_KILN_SETUP",
            .password = "",
            .ssid_len = strlen("TRAE_KILN_SETUP"),
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    // Set mode to APSTA so we can scan
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); 
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s",
             "TRAE_KILN_SETUP", "");
}

std::string wifi_scan_networks(void) {
    // Ensure we are in a mode that supports scanning (STA or APSTA)
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 100,
                .max = 300
            },
        }
    };
    
    ESP_LOGI(TAG, "Starting WiFi Scan...");
    esp_wifi_scan_start(&scan_config, true); // Blocking scan
    
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    
    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(ap_num * sizeof(wifi_ap_record_t));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_num, ap_list));
    
    ESP_LOGI(TAG, "Scan done. Found %d APs", ap_num);
    
    DynamicJsonDocument doc(2048);
    JsonArray array = doc.to<JsonArray>();

    for (int i = 0; i < ap_num; i++) {
        // Filter out empty SSIDs
        if (strlen((char *)ap_list[i].ssid) > 0) {
            JsonObject obj = array.createNestedObject();
            obj["ssid"] = (char *)ap_list[i].ssid;
            obj["rssi"] = ap_list[i].rssi;
            obj["auth"] = ap_list[i].authmode;
        }
    }
    
    free(ap_list);
    
    std::string output;
    serializeJson(doc, output);
    return output;
}
