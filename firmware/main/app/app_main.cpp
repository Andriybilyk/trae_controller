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
#include "esp_ota_ops.h"
#include "mdns.h"
#include <cstring>
#include <atomic>
#include <string>

#include "app/device_commands.h"
#include "cJSON.h"
#include "kiln_config/config.h"
#include "kiln_control/thermal_control.h"
#include "kiln_hal_heater/heater_hal.h"
#include "slint_ui.h"
#include "ui/slint_bridge.h"
#include "net/wifi_server.h"
#include "net/wifi_connection.h"
#include "net/remote_access.h"
#include "drivers/fan_driver.h"
#include "drivers/buzzer_driver.h"
#include "drivers/rtc_ds3231.h"
#include "kiln_config/config_store.h"
#include "kiln_config/fs_utils.h"
#include "config/board_profile.h"

static const char *TAG = "MAIN";
static constexpr uint32_t UI_TASK_STACK_BYTES = 32768;
static constexpr uint64_t UI_HEARTBEAT_TIMEOUT_MS = 4000;
static constexpr uint64_t NET_HEARTBEAT_TIMEOUT_MS = 15000;

static TaskHandle_t s_ui_task = nullptr;
static std::atomic<uint64_t> s_server_heartbeat_ms{0};
static std::atomic<bool> s_server_watchdog_armed{false};
static std::atomic<float> s_board_temp_c{25.0f};
static temperature_sensor_handle_t s_board_temp_sensor = nullptr;

static const char* reset_reason_to_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_UNKNOWN: return "UNKNOWN";
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXT";
        case ESP_RST_SW: return "SW";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        case ESP_RST_USB: return "USB";
        case ESP_RST_JTAG: return "JTAG";
        case ESP_RST_EFUSE: return "EFUSE";
        case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
        case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
        default: return "UNMAPPED";
    }
}

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
    // slint_ui_run() is a long-running blocking loop; this task cannot feed TWDT directly.
    // Keep UI watchdoging via heartbeat in control_task instead of task_wdt registration.
    slint_bridge_ui_heartbeat();

    slint_ui_run();
}

static void control_task(void *arg) {
    (void)arg;
    ESP_LOGI("CTRL_TASK", "Starting control task");
    const bool wdt_registered = (esp_task_wdt_add(nullptr) == ESP_OK);
    const bool is_new_p4 = (board_profile::current_board() == board_profile::BoardId::NewP4);

    uint64_t lastLog = 0;
    bool watchdog_latched = false;
    uint64_t last_net_stall_log_ms = 0;
    int prev_step = -1;
    bool prev_firing = false;
    KilnStatus prev_status = KILN_IDLE;

    while (true) {
        if (wdt_registered) {
            (void)esp_task_wdt_reset();
        }
        thermalCtrl.loop();
        const KilnState st = thermalCtrl.getState();
        if (!is_new_p4) {
            fan_driver_update_from_temperature(board_temp_read_c(), st.isFiring);
            buzzer_driver_tick();
            if (prev_firing && st.isFiring && st.currentStep != prev_step && st.currentStep >= 0) {
                buzzer_driver_beep_segment();
            }
            if (prev_status != KILN_COMPLETE && st.status == KILN_COMPLETE) {
                buzzer_driver_beep_complete();
            }
        }
        prev_step = st.currentStep;
        prev_firing = st.isFiring;
        prev_status = st.status;

        const uint64_t now = esp_timer_get_time() / 1000ULL; // ms
        const uint64_t ui_hb = slint_bridge_get_ui_heartbeat_ms();
        const uint64_t net_hb = s_server_heartbeat_ms.load(std::memory_order_relaxed);

        const bool ui_stalled = (ui_hb > 0) && (now > ui_hb) && ((now - ui_hb) > UI_HEARTBEAT_TIMEOUT_MS);
        const bool net_watchdog_armed = s_server_watchdog_armed.load(std::memory_order_relaxed);
        const bool net_stalled = net_watchdog_armed &&
                                 (net_hb > 0) &&
                                 (now > net_hb) &&
                                 ((now - net_hb) > NET_HEARTBEAT_TIMEOUT_MS);

        if (net_stalled && !ui_stalled) {
            if (now - last_net_stall_log_ms >= 5000) {
                ESP_LOGW("CTRL_TASK", "NET heartbeat stalled (ignoring), now=%llu hb=%llu", (unsigned long long)now, (unsigned long long)net_hb);
                last_net_stall_log_ms = now;
            }
            s_server_heartbeat_ms.store(now, std::memory_order_relaxed);
        }

        if (!watchdog_latched && ui_stalled) {
            watchdog_latched = true;
            heater_hal_all_off();
            if (ui_stalled && net_stalled) {
                thermalCtrl.emergencyStop("Watchdog: UI+NET stalled");
            } else if (ui_stalled) {
                thermalCtrl.emergencyStop("Watchdog: UI stalled");
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
    s_server_watchdog_armed.store(false, std::memory_order_relaxed);
    s_server_heartbeat_ms.store((uint64_t)(esp_timer_get_time() / 1000ULL), std::memory_order_relaxed);

    wifiServer.begin();
    remote_access_init();
    s_server_heartbeat_ms.store((uint64_t)(esp_timer_get_time() / 1000ULL), std::memory_order_relaxed);
    s_server_watchdog_armed.store(true, std::memory_order_relaxed);

    uint64_t lastLog = 0;

    while (true) {
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
    // Confirm OTA update if we booted successfully
    esp_ota_mark_app_valid_cancel_rollback();

    const esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d (%s), free_heap=%u, min_free_heap=%u",
             (int)rr,
             reset_reason_to_str(rr),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());

    esp_task_wdt_config_t twdt_config = {};
    twdt_config.timeout_ms = 12000;
    twdt_config.idle_core_mask = 0;
    twdt_config.trigger_panic = false;
    (void)esp_task_wdt_init(&twdt_config);

    // Initialize NVS (needed for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "TRAE KILN CONTROLLER STARTING...");
    const bool is_new_p4 = (board_profile::current_board() == board_profile::BoardId::NewP4);
    if (is_new_p4) {
        // Keep relay control pin in safe input state during early boot and display bring-up.
        heater_hal_set_display_ready(false);
    }
    board_temp_init();
    if (!is_new_p4) {
        (void)rtc_ds3231_init();
        if (rtc_ds3231_apply_time_to_system()) {
            ESP_LOGI(TAG, "Time restored from DS3231");
        } else {
            ESP_LOGW(TAG, "DS3231 time not available yet");
        }
    } else {
        ESP_LOGI(TAG, "Skip DS3231 init for NewP4 profile");
    }

    // Init fan/buzzer only on non-P4 boards during P4 display bring-up.
    if (!is_new_p4) {
        fan_driver_init();
        fan_driver_set_manual(false);
        fan_driver_set_power_percent(0);
        buzzer_driver_init();
    } else {
        ESP_LOGI(TAG, "Skip fan+buzzer init for NewP4 profile");
    }

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
        kiln_config_restore_json_config_file();
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

        const std::string cfg = kiln_fs_read_text(CONFIG_FILE, false);
        if (!cfg.empty()) {
            cJSON *root = cJSON_Parse(cfg.c_str());
            if (root && cJSON_IsObject(root)) {
                device_commands::FanConfig fan_cfg = device_commands::current_fan_config();

                const cJSON *fanManual = cJSON_GetObjectItem(root, "fan_manual");
                if (cJSON_IsBool(fanManual)) fan_cfg.manual = cJSON_IsTrue(fanManual);
                const cJSON *fanAuto = cJSON_GetObjectItem(root, "fan_auto");
                if (cJSON_IsBool(fanAuto)) fan_cfg.auto_enabled = cJSON_IsTrue(fanAuto);
                const cJSON *fanPower = cJSON_GetObjectItem(root, "fan_power");
                if (cJSON_IsNumber(fanPower)) {
                    int v = (int)fanPower->valuedouble;
                    if (v < 0) v = 0;
                    if (v > 100) v = 100;
                    fan_cfg.power = (uint8_t)v;
                }
                const cJSON *fanTMin = cJSON_GetObjectItem(root, "fan_temp_min_c");
                if (cJSON_IsNumber(fanTMin)) fan_cfg.temp_min_c = (float)fanTMin->valuedouble;
                const cJSON *fanTMax = cJSON_GetObjectItem(root, "fan_temp_max_c");
                if (cJSON_IsNumber(fanTMax)) fan_cfg.temp_max_c = (float)fanTMax->valuedouble;
                const cJSON *fanPMin = cJSON_GetObjectItem(root, "fan_power_min");
                if (cJSON_IsNumber(fanPMin)) {
                    int v = (int)fanPMin->valuedouble;
                    if (v < 0) v = 0;
                    if (v > 100) v = 100;
                    fan_cfg.power_min = (uint8_t)v;
                }
                const cJSON *fanPMax = cJSON_GetObjectItem(root, "fan_power_max");
                if (cJSON_IsNumber(fanPMax)) {
                    int v = (int)fanPMax->valuedouble;
                    if (v < 0) v = 0;
                    if (v > 100) v = 100;
                    fan_cfg.power_max = (uint8_t)v;
                }

                device_commands::apply_fan_config(fan_cfg);
            }
            if (root) cJSON_Delete(root);
        }
    }

    ESP_LOGI(TAG, "Init Thermal Control...");
    thermalCtrl.begin();
    // Do not call stop() immediately, as it clears the recovery state.
    // Instead, check if it was already recovered in begin()
    if (thermalCtrl.getState().isFiring == false) {
        thermalCtrl.stop("System Boot");
    }

    // Network services are enabled only when a real Wi-Fi backend is compiled in.
    const bool network_enabled = wifi_backend_available();
    if (network_enabled) {
        // 6. Initialize mDNS so we can resolve http://kiln.local
        esp_err_t err = mdns_init();
        if (err == ESP_OK) {
            mdns_hostname_set("kiln");
            mdns_instance_name_set("Trae Kiln Controller");
            mdns_service_add("Kiln Web UI", "_http", "_tcp", 80, NULL, 0);
            ESP_LOGI(TAG, "mDNS initialized. Hostname: kiln.local");
        } else {
            ESP_LOGE(TAG, "mDNS init failed: %d", err);
        }
    } else {
        ESP_LOGW(TAG, "Network stack disabled (board=%s, wifi_backend=%s)",
                 board_profile::current_board_name(),
                 wifi_backend_name());
    }

    ESP_LOGI(TAG, "Setup Complete.");

    // Start runtime tasks (reduce main task stack pressure)
    ESP_LOGI(TAG, "Starting tasks...");

    // Slint + Rust std (software renderer) is stack-hungry during layout/render and key handling.
    // Keep a larger stack to prevent overflow when opening complex overlays/keyboards.
    // Priority is increased to 10 on NewP4 to ensure smooth display refresh despite background WiFi/SDIO activity.
    (void)xTaskCreatePinnedToCore(ui_task, "ui", UI_TASK_STACK_BYTES, nullptr, 10, &s_ui_task, 1);

    // Control loop task (safety-critical: keep running even if UI stalls).
    (void)xTaskCreatePinnedToCore(control_task, "ctrl", 4096, nullptr, 11, nullptr, 1);

    // Networking + periodic WS broadcast task.
    if (network_enabled) {
        (void)xTaskCreatePinnedToCore(server_task, "srv", 6144, nullptr, 6, nullptr, 0);
    }

    ESP_LOGI(TAG, "Main task done; deleting main task to avoid stack overflow.");
    vTaskDelete(NULL);
}
