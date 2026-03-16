#include <stdio.h>
#include "esp_log.h"
#include "esp_littlefs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_system.h"
#include <cstring>

#include "kiln_config/config.h"
#include "kiln_control/thermal_control.h"
#include "slint_ui.h"
#include "net/wifi_server.h"

static const char *TAG = "MAIN";

static TaskHandle_t s_ui_task = nullptr;

static void ui_task(void *arg) {
    (void)arg;
    ESP_LOGI("UI_TASK", "Starting UI task");

    slint_ui_run();
}

static void control_task(void *arg) {
    (void)arg;
    ESP_LOGI("CTRL_TASK", "Starting control task");

    uint64_t lastLog = 0;

    while (true) {
        thermalCtrl.loop();

        const uint64_t now = esp_timer_get_time() / 1000ULL; // ms
        if (now - lastLog >= 5000) {
            lastLog = now;
            ESP_LOGI("CTRL_TASK", "Stack watermark: %u words", (unsigned)uxTaskGetStackHighWaterMark(NULL));
            if (s_ui_task) {
                ESP_LOGI("CTRL_TASK", "UI stack watermark: %u words", (unsigned)uxTaskGetStackHighWaterMark(s_ui_task));
            }
        }

        // 10 Hz control loop
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void server_task(void *arg) {
    (void)arg;
    ESP_LOGI("SRV_TASK", "Starting server task");

    wifiServer.begin();

    uint64_t lastLog = 0;

    while (true) {
        wifiServer.loop();

        const uint64_t now = esp_timer_get_time() / 1000ULL; // ms
        if (now - lastLog >= 5000) {
            lastLog = now;
            ESP_LOGI("SRV_TASK", "Stack watermark: %u words", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

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
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/littlefs";
    conf.partition_label = "storage";
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;

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

        FILE *sf = fopen("/littlefs/schedules.json", "r");
        if (sf) {
            fclose(sf);
        } else {
            sf = fopen("/littlefs/schedules.json", "w");
            if (sf) {
                const char *empty = "[]";
                fwrite(empty, 1, strlen(empty), sf);
                fclose(sf);
            }
        }

        FILE *hf = fopen("/littlefs/history.json", "r");
        if (hf) {
            fclose(hf);
        } else {
            hf = fopen("/littlefs/history.json", "w");
            if (hf) {
                const char *empty = "[]";
                fwrite(empty, 1, strlen(empty), hf);
                fclose(hf);
            }
        }
    }

    // Init Thermal Control
    ESP_LOGI(TAG, "Init Thermal Control...");
    thermalCtrl.begin();
    thermalCtrl.stop("System Boot");

    ESP_LOGI(TAG, "Setup Complete.");

    // Start runtime tasks (reduce main task stack pressure)
    ESP_LOGI(TAG, "Starting tasks...");

    // Slint + Rust std (software renderer) is stack-hungry during layout/render.
    // If this is too small, we can corrupt the scheduler and crash in FreeRTOS internals.
    (void)xTaskCreatePinnedToCore(ui_task, "ui", 16384, nullptr, 6, &s_ui_task, 1);

    // Control loop task (safety-critical: keep running even if UI stalls).
    (void)xTaskCreatePinnedToCore(control_task, "ctrl", 4096, nullptr, 8, nullptr, 1);

    // Networking + periodic WS broadcast task.
    (void)xTaskCreatePinnedToCore(server_task, "srv", 6144, nullptr, 5, nullptr, 0);

    ESP_LOGI(TAG, "Main task done; deleting main task to avoid stack overflow.");
    vTaskDelete(NULL);
}
