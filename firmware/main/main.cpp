#include <stdio.h>
#include "esp_log.h"
#include "esp_littlefs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "config.h"
#include "thermal_control.h"
#include "ui_manager.h"
#include "wifi_server.h"

static const char *TAG = "MAIN";

extern "C" void app_main(void) {
    // Initialize NVS (needed for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "TRAE KILN CONTROLLER STARTING...");

    // Init LittleFS
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false
    };

    ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGI(TAG, "LittleFS Mounted");
        // Verify index.html exists
        FILE* f = fopen("/littlefs/index.html", "r");
        if (f == NULL) {
            ESP_LOGE(TAG, "CRITICAL: /littlefs/index.html NOT FOUND!");
        } else {
            fclose(f);
        }
    }

    // Init Thermal Control
    ESP_LOGI(TAG, "Init Thermal Control...");
    thermalCtrl.begin();

    // Init UI
    ESP_LOGI(TAG, "Init UI Manager...");
    uiManager.begin();

    // Init WiFi & Server
    ESP_LOGI(TAG, "Init WiFi & Server...");
    wifiServer.begin();

    ESP_LOGI(TAG, "Setup Complete.");

    // Main loop equivalent
    uint64_t lastControl = 0;
    uint64_t lastHeartbeat = 0;

    while (1) {
        // 1. UI Update (LVGL)
        uiManager.loop();
        // Yield to allow other tasks to run
        vTaskDelay(pdMS_TO_TICKS(5));

        // 2. WiFi Server Loop
        wifiServer.loop();
        vTaskDelay(pdMS_TO_TICKS(5));

        // 3. Control Loop (10Hz)
        uint64_t now = esp_timer_get_time() / 1000; // ms
        if (now - lastControl > 100) {
            lastControl = now;
            thermalCtrl.loop();
            uiManager.updateStatus(thermalCtrl.getState());
        }

        // 4. Heartbeat (5 seconds)
        if (now - lastHeartbeat > 5000) {
            lastHeartbeat = now;
            ESP_LOGI(TAG, "Heartbeat. Heap: %" PRIu32, esp_get_free_heap_size());
        }
    }
}
