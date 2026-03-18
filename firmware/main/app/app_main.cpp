#include <stdio.h>
#include "esp_log.h"
#include "esp_littlefs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "driver/temperature_sensor.h"
#include <cstring>
#include <atomic>

#include "kiln_config/config.h"
#include "kiln_control/thermal_control.h"
#include "kiln_hal_heater/heater_hal.h"
#include "slint_ui.h"
#include "ui/slint_bridge.h"
#include "net/wifi_server.h"
#include "net/remote_access.h"
#include "drivers/fan_driver.h"

static const char *TAG = "MAIN";
static constexpr uint32_t UI_TASK_STACK_BYTES = 32768;
static constexpr uint64_t UI_HEARTBEAT_TIMEOUT_MS = 4000;
static constexpr uint64_t NET_HEARTBEAT_TIMEOUT_MS = 4000;

static TaskHandle_t s_ui_task = nullptr;
static std::atomic<uint64_t> s_server_heartbeat_ms{0};
static std::atomic<bool> s_server_watchdog_armed{false};
static std::atomic<float> s_board_temp_c{25.0f};
static temperature_sensor_handle_t s_board_temp_sensor = nullptr;

static void board_temp_init(void) {
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &s_board_temp_sensor) != ESP_OK || !s_board_temp_sensor) {
        ESP_LOGW(TAG, "Board temp sensor install failed");
        s_board_temp_sensor = nullptr;
        return;
    }
    if (temperature_sensor_enable(s_board_temp_sensor) != ESP_OK) {
        ESP_LOGW(TAG, "Board temp sensor enable failed");
        (void)temperature_sensor_uninstall(s_board_temp_sensor);
        s_board_temp_sensor = nullptr;
        return;
    }
    ESP_LOGI(TAG, "Board temp sensor enabled");
}

static float board_temp_read_c(void) {
    float out = s_board_temp_c.load(std::memory_order_relaxed);
    if (!s_board_temp_sensor) return out;
    float sample = out;
    if (temperature_sensor_get_celsius(s_board_temp_sensor, &sample) == ESP_OK) {
        if (sample > -40.0f && sample < 125.0f) {
            out = sample;
            s_board_temp_c.store(out, std::memory_order_relaxed);
        }
    }
    return out;
}

static void ui_task(void *arg) {
    (void)arg;
    ESP_LOGI("UI_TASK", "Starting UI task");
    (void)esp_task_wdt_add(nullptr);
    slint_bridge_ui_heartbeat();

    slint_ui_run();
}

static void control_task(void *arg) {
    (void)arg;
    ESP_LOGI("CTRL_TASK", "Starting control task");
    (void)esp_task_wdt_add(nullptr);

    uint64_t lastLog = 0;
    bool watchdog_latched = false;

    while (true) {
        (void)esp_task_wdt_reset();
        thermalCtrl.loop();
        const KilnState st = thermalCtrl.getState();
        fan_driver_update_from_temperature(board_temp_read_c(), st.isFiring);

        const uint64_t now = esp_timer_get_time() / 1000ULL; // ms
        const uint64_t ui_hb = slint_bridge_get_ui_heartbeat_ms();
        const uint64_t net_hb = s_server_heartbeat_ms.load(std::memory_order_relaxed);

        const bool ui_stalled = (ui_hb > 0) && (now > ui_hb) && ((now - ui_hb) > UI_HEARTBEAT_TIMEOUT_MS);
        const bool net_watchdog_armed = s_server_watchdog_armed.load(std::memory_order_relaxed);
        const bool net_stalled = net_watchdog_armed &&
                                 (net_hb > 0) &&
                                 (now > net_hb) &&
                                 ((now - net_hb) > NET_HEARTBEAT_TIMEOUT_MS);

        if (!watchdog_latched && (ui_stalled || net_stalled)) {
            watchdog_latched = true;
            heater_hal_all_off();
            if (ui_stalled && net_stalled) {
                thermalCtrl.emergencyStop("Watchdog: UI+NET stalled");
            } else if (ui_stalled) {
                thermalCtrl.emergencyStop("Watchdog: UI stalled");
            } else {
                thermalCtrl.emergencyStop("Watchdog: NET stalled");
            }
            ESP_LOGE("CTRL_TASK", "Watchdog tripped, heater forced OFF");
        }
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
    (void)esp_task_wdt_add(nullptr);
    s_server_watchdog_armed.store(false, std::memory_order_relaxed);
    s_server_heartbeat_ms.store((uint64_t)(esp_timer_get_time() / 1000ULL), std::memory_order_relaxed);

    wifiServer.begin();
    remote_access_init();
    s_server_heartbeat_ms.store((uint64_t)(esp_timer_get_time() / 1000ULL), std::memory_order_relaxed);
    s_server_watchdog_armed.store(true, std::memory_order_relaxed);

    uint64_t lastLog = 0;

    while (true) {
        (void)esp_task_wdt_reset();
        wifiServer.loop();
        remote_access_loop();
        s_server_heartbeat_ms.store((uint64_t)(esp_timer_get_time() / 1000ULL), std::memory_order_relaxed);

        const uint64_t now = esp_timer_get_time() / 1000ULL; // ms
        if (now - lastLog >= 5000) {
            lastLog = now;
            ESP_LOGI("SRV_TASK", "Stack watermark: %u words", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

extern "C" void app_main(void) {
    esp_task_wdt_config_t twdt_config = {};
    twdt_config.timeout_ms = 12000;
    twdt_config.idle_core_mask = 0;
    twdt_config.trigger_panic = true;
    (void)esp_task_wdt_init(&twdt_config);

    // Initialize NVS (needed for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "TRAE KILN CONTROLLER STARTING...");
    board_temp_init();

    // Init fan PWM (manual override from UI; default OFF).
    fan_driver_init();
    fan_driver_set_manual(false);
    fan_driver_set_power_percent(0);

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

    // Slint + Rust std (software renderer) is stack-hungry during layout/render and key handling.
    // Keep a larger stack to prevent overflow when opening complex overlays/keyboards.
    (void)xTaskCreatePinnedToCore(ui_task, "ui", UI_TASK_STACK_BYTES, nullptr, 6, &s_ui_task, 1);

    // Control loop task (safety-critical: keep running even if UI stalls).
    (void)xTaskCreatePinnedToCore(control_task, "ctrl", 4096, nullptr, 8, nullptr, 1);

    // Networking + periodic WS broadcast task.
    (void)xTaskCreatePinnedToCore(server_task, "srv", 6144, nullptr, 5, nullptr, 0);

    ESP_LOGI(TAG, "Main task done; deleting main task to avoid stack overflow.");
    vTaskDelete(NULL);
}
