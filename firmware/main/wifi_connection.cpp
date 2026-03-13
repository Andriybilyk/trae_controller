#include "wifi_connection.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "cJSON.h"

static const char *TAG = "WIFI_CONN";

// --- NVS Storage ---
#define NVS_NAMESPACE "wifi_config"
#define KEY_SSID      "ssid"
#define KEY_PASS      "password"

// --- Scan Task ---
static TaskHandle_t wifi_scan_task_handle = NULL;
static SemaphoreHandle_t scan_done_sem = NULL;
static std::string scan_result_json;

static void wifi_scan_task(void *pvParameters) {
    ESP_LOGI(TAG, "Starting WiFi Scan Task...");
    
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 800 } }
    };

    if (esp_wifi_scan_start(&scan_config, true) == ESP_OK) {
        uint16_t ap_num = 0;
        esp_wifi_scan_get_ap_num(&ap_num);
        if (ap_num > 0) {
            wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(ap_num * sizeof(wifi_ap_record_t));
            if (ap_list && esp_wifi_scan_get_ap_records(&ap_num, ap_list) == ESP_OK) {
                cJSON *root = cJSON_CreateArray();
                for (int i = 0; i < ap_num; i++) {
                    if (strlen((char *)ap_list[i].ssid) == 0) continue;

                    cJSON *obj = cJSON_CreateObject();
                    cJSON_AddStringToObject(obj, "ssid", (char *)ap_list[i].ssid);
                    cJSON_AddNumberToObject(obj, "rssi", ap_list[i].rssi);
                    cJSON_AddNumberToObject(obj, "auth", ap_list[i].authmode);
                    cJSON_AddItemToArray(root, obj);
                }

                char *rendered = cJSON_PrintUnformatted(root);
                scan_result_json.assign(rendered ? rendered : "[]");
                if (rendered) free(rendered);
                cJSON_Delete(root);
            }
            free(ap_list);
        } else {
            scan_result_json = "[]";
        }
    } else {
        scan_result_json = "[]";
        ESP_LOGE(TAG, "WiFi scan failed to start.");
    }
    
    ESP_LOGI(TAG, "WiFi Scan completed.");
    xSemaphoreGive(scan_done_sem);
    vTaskDelete(NULL); // Delete self
}

void wifi_start_scan(void) {
    if (scan_done_sem == NULL) {
        scan_done_sem = xSemaphoreCreateBinary();
    }
    // Take semaphore before starting, so we can wait for it
    xSemaphoreTake(scan_done_sem, 0); 
    
    // Ensure APSTA mode for scanning
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    
    xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 5, &wifi_scan_task_handle);
}

bool wifi_is_scan_done(void) {
    if (scan_done_sem == NULL) return false;
    return xSemaphoreTake(scan_done_sem, 0) == pdTRUE;
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

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, ssid);
    strcpy((char*)wifi_config.sta.password, pass);
    
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    vEventGroupDelete(s_wifi_event_group);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", ssid);
        return true;
    } else {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", ssid);
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }
}

// --- AP Mode ---
static void ap_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac), event->aid);
    }
}

void wifi_init_softap(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &ap_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "TRAE_KILN_SETUP",
            .password = "",
            .ssid_len = strlen("TRAE_KILN_SETUP"),
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
            .pmf_cfg = { .required = false },
        },
    };

    esp_wifi_set_mode(WIFI_MODE_APSTA); 
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:TRAE_KILN_SETUP");
}

// Kept for compatibility, but it's now a blocking wrapper. Avoid using in handlers.
std::string wifi_scan_networks(void) {
    wifi_start_scan();
    if(scan_done_sem) xSemaphoreTake(scan_done_sem, portMAX_DELAY); // Block until done
    return wifi_get_scanned_networks();
}
