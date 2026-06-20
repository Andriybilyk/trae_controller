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
#include "ui/ui_backend.h"
#include "net/wifi_server.h"
#include "net/wifi_connection.h"
#include "net/remote_access.h"
#include "drivers/fan_driver.h"
#include "drivers/buzzer_driver.h"
#include "drivers/rtc_ds3231.h"
#include "kiln_config/config_store.h"
#include "kiln_config/fs_utils.h"
#include "config/board_profile.h"
#include "ui.h"

static const char *TAG = "MAIN";
static constexpr uint32_t UI_TASK_STACK_BYTES = 32768;
static constexpr uint64_t UI_HEARTBEAT_TIMEOUT_MS = 12000;
static constexpr uint64_t NET_HEARTBEAT_TIMEOUT_MS = 15000;
static constexpr bool kNewP4UiOnlyDiag = false;
// Allow networking even in UI-only diagnostics so Wi-Fi stack can be validated
// without enabling thermal/control logic.
static constexpr bool kNewP4UiOnlyDiagEnableNetwork = true;

static TaskHandle_t s_ui_task = nullptr;
static std::atomic<uint64_t> s_server_heartbeat_ms{0};
static std::atomic<bool> s_server_watchdog_armed{false};
static std::atomic<float> s_board_temp_c{25.0f};
static temperature_sensor_handle_t s_board_temp_sensor = nullptr;

extern "C" float kiln_ui_get_current_temp_c(void) {
    const KilnState st = thermalCtrl.getState();
    return st.currentTemp;
}

extern "C" float kiln_ui_get_target_temp_c(void) {
    const KilnState st = thermalCtrl.getState();
    return st.targetTemp;
}

extern "C" int kiln_ui_get_is_firing(void) {
    const KilnState st = thermalCtrl.getState();
    return st.isFiring ? 1 : 0;
}

extern "C" int kiln_ui_get_status(void) {
    const KilnState st = thermalCtrl.getState();
    return (int)st.status;
}

extern "C" int kiln_ui_get_current_step(void) {
    const KilnState st = thermalCtrl.getState();
    return st.currentStep;
}

extern "C" int kiln_ui_get_total_steps(void) {
    const KilnState st = thermalCtrl.getState();
    return st.totalSteps;
}

extern "C" int kiln_ui_get_active_step_type(void) {
    int type = -1;
    (void)thermalCtrl.getUiActiveStepInfo(&type, NULL, NULL, NULL);
    return type;
}

extern "C" float kiln_ui_get_active_step_target_c(void) {
    float target = 0.0f;
    (void)thermalCtrl.getUiActiveStepInfo(NULL, &target, NULL, NULL);
    return target;
}

extern "C" float kiln_ui_get_active_step_rate_c_per_min(void) {
    float rate = 0.0f;
    (void)thermalCtrl.getUiActiveStepInfo(NULL, NULL, &rate, NULL);
    return rate;
}

extern "C" float kiln_ui_get_active_step_start_temp_c(void) {
    float start_temp = 0.0f;
    (void)thermalCtrl.getUiActiveStepInfo(NULL, NULL, NULL, &start_temp);
    return start_temp;
}

extern "C" int kiln_ui_get_thermocouple_status(void) {
    const ThermoStats ts = thermalCtrl.getThermoStats();
    const uint32_t raw = ts.raw;
    const bool raw_all_zero = (raw == 0x00000000U);
    const bool raw_all_ones = (raw == 0xFFFFFFFFU);
    if (ts.readCount == 0 || raw_all_zero || raw_all_ones) return 3;

    const bool raw_fault = (raw & 0x00010000U) != 0;
    const bool raw_open = raw_fault && ((raw & 0x00000001U) != 0);
    const bool raw_short = raw_fault && ((raw & 0x00000006U) != 0);
    if (raw_open) return 1;
    if (raw_short) return 2;
    if (raw_fault) return 4;
    return 0;
}

extern "C" float kiln_ui_get_temperature_offset_c(void) {
    return thermalCtrl.getTemperatureOffset();
}

extern "C" int kiln_ui_set_temperature_offset_c(float offset_c) {
    return thermalCtrl.setTemperatureOffset(offset_c) ? 1 : 0;
}

extern "C" float kiln_ui_get_pid_profile_value(int high_profile, int term) {
    double kp = 0.0, ki = 0.0, kd = 0.0;
    thermalCtrl.getPidProfileTunings(high_profile != 0, &kp, &ki, &kd);
    switch (term) {
        case 0: return (float)kp;
        case 1: return (float)ki;
        case 2: return (float)kd;
        default: return 0.0f;
    }
}

extern "C" int kiln_ui_set_pid_profile_value(int high_profile, int term, float value) {
    double kp = 0.0, ki = 0.0, kd = 0.0;
    thermalCtrl.getPidProfileTunings(high_profile != 0, &kp, &ki, &kd);
    switch (term) {
        case 0: kp = value; break;
        case 1: ki = value; break;
        case 2: kd = value; break;
        default: return 0;
    }
    return thermalCtrl.setPidProfileTunings(high_profile != 0, kp, ki, kd) ? 1 : 0;
}

extern "C" int kiln_ui_set_manual_ssr_test(int on) {
    return thermalCtrl.setManualSsrTest(on != 0) ? 1 : 0;
}

extern "C" int kiln_ui_get_manual_ssr_test(void) {
    return thermalCtrl.getManualSsrTest() ? 1 : 0;
}

extern "C" int kiln_ui_fault_is_active(void) {
    const SafetyStats ss = thermalCtrl.getSafetyStats();
    return ss.faultActive ? 1 : 0;
}

extern "C" uint32_t kiln_ui_fault_code(void) {
    const SafetyStats ss = thermalCtrl.getSafetyStats();
    return ss.faultCode;
}

extern "C" uint64_t kiln_ui_fault_since_ms(void) {
    const SafetyStats ss = thermalCtrl.getSafetyStats();
    return ss.faultSinceMs;
}

extern "C" int kiln_ui_fault_reason(char *out_buf, int out_buf_len) {
    if (!out_buf || out_buf_len <= 0) return 0;
    out_buf[0] = 0;
    const SafetyStats ss = thermalCtrl.getSafetyStats();
    const size_t max_copy = (size_t)out_buf_len - 1U;
    strncpy(out_buf, ss.faultReason, max_copy);
    out_buf[max_copy] = 0;
    return (int)strlen(out_buf);
}

extern "C" int kiln_ui_clear_fault(void) {
    return thermalCtrl.clearFault() ? 1 : 0;
}

extern "C" int kiln_ui_start_pid_autotune(void) {
    return thermalCtrl.startAutotune(thermalCtrl.getAutotuneTargetC()) ? 1 : 0;
}

extern "C" void kiln_ui_stop_pid_autotune(void) {
    thermalCtrl.stopAutotune("User stopped autotune");
}

extern "C" int kiln_ui_get_pid_autotune_active(void) {
    const ThermalController::AutoTuneStatus tune = thermalCtrl.getAutotuneStatus();
    return tune.active ? 1 : 0;
}

extern "C" int kiln_ui_get_pid_autotune_cycles(void) {
    const ThermalController::AutoTuneStatus tune = thermalCtrl.getAutotuneStatus();
    return tune.cycles;
}

extern "C" int kiln_ui_get_pid_autotune_total_cycles(void) {
    const ThermalController::AutoTuneStatus tune = thermalCtrl.getAutotuneStatus();
    return tune.total_cycles;
}

extern "C" float kiln_ui_get_pid_autotune_target_c(void) {
    return thermalCtrl.getAutotuneTargetC();
}

extern "C" int kiln_ui_start_selected_program(void) {
    
    const uint8_t id = ui_get_selected_program_id();
    if (id == 255 || id >= ui_get_program_count()) return (int)device_commands::ResultCode::NoSchedule;

    const uint16_t total = ui_get_program_steps_total(id);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", ui_get_program_name(id));
    cJSON *steps = cJSON_AddArrayToObject(root, "steps");

    for (uint16_t i = 0; i < total; i++) {
        int16_t temp_c = ui_get_program_step_temp(id, i);
        uint16_t rate_c_per_h_x10 = ui_get_program_step_rate_c_per_h_x10(id, i);
        const uint32_t hold_s = ui_get_program_step_hold_s(id, i);

        if (temp_c == 0) {
            temp_c = (i == 0) ? 200 : (i == 1) ? 600 : (i == 2) ? 900 : 1100;
        }
        if (rate_c_per_h_x10 == 0) {
            rate_c_per_h_x10 = (i == 0) ? 1500 : (i == 1) ? 2000 : (i == 2) ? 1800 : 1500;
        }

        cJSON *ramp = cJSON_CreateObject();
        cJSON_AddStringToObject(ramp, "type", "ramp");
        cJSON_AddNumberToObject(ramp, "target", (double)temp_c);
        const double rate_c_per_h = (double)rate_c_per_h_x10 / 10.0;
        const double rate_c_per_min = rate_c_per_h / 60.0;
        cJSON_AddNumberToObject(ramp, "rate", rate_c_per_min);
        cJSON_AddItemToArray(steps, ramp);

        if (hold_s > 0) {
            cJSON *hold = cJSON_CreateObject();
            cJSON_AddStringToObject(hold, "type", "hold");
            cJSON_AddNumberToObject(hold, "holdTime", (double)hold_s / 60.0);
            cJSON_AddItemToArray(steps, hold);
        }
    }

    char *rendered = cJSON_PrintUnformatted(root);
    std::string json = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(root);

    const device_commands::CommandResult res = device_commands::start_from_schedule_json(json);
    return (int)res.code;
}

extern "C" void kiln_ui_pause_firing(void) {
    thermalCtrl.stop("Pause");
}

extern "C" void kiln_ui_stop_firing(void) {
    thermalCtrl.stop("Stop");
}

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

    ui_backend_run();
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
        const uint64_t ui_hb = now; // LVGL heartbeat placeholder for now
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

    // Defer network bootstrap until UI has entered its render/event loop.
    // This avoids racing Wi-Fi backend bring-up against first-frame UI init.
    const uint64_t wait_start_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    while (true) {
        const uint64_t hb_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        if (hb_ms > 0) {
            break;
        }
        if (now_ms - wait_start_ms > 6000ULL) {
            ESP_LOGW("SRV_TASK", "UI heartbeat wait timed out, continuing network bootstrap");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(250));
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
    twdt_config.timeout_ms = 30000;
    twdt_config.idle_core_mask = 0;
    twdt_config.trigger_panic = false;
    const esp_err_t twdt_err = esp_task_wdt_init(&twdt_config);
    if (twdt_err == ESP_ERR_INVALID_STATE) {
        (void)esp_task_wdt_reconfigure(&twdt_config);
    }

    // Initialize NVS (needed for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "TRAE KILN CONTROLLER STARTING...");
    const bool is_new_p4 = (board_profile::current_board() == board_profile::BoardId::NewP4);
    const bool ui_only_diag = (is_new_p4 && kNewP4UiOnlyDiag);
    if (is_new_p4) {
        // Keep relay control pin in safe input state during early boot and display bring-up.
        heater_hal_set_display_ready(false);
        heater_hal_all_off();
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

    if (!ui_only_diag) {
        ESP_LOGI(TAG, "Init Thermal Control...");
        thermalCtrl.begin();
        // Do not call stop() immediately, as it clears the recovery state.
        // Instead, check if it was already recovered in begin()
        if (thermalCtrl.getState().isFiring == false) {
            thermalCtrl.stop("System Boot");
        }
    } else {
        ESP_LOGW(TAG, "NewP4 UI-only diagnostic mode enabled: thermal/network/control disabled");
    }

    // In NewP4 UI-only diagnostics we can still run network stack to validate
    // Wi-Fi behavior while thermal/control stay disabled.
    const bool allow_network_in_diag = ui_only_diag && kNewP4UiOnlyDiagEnableNetwork;
    bool network_enabled = ((!ui_only_diag) || allow_network_in_diag) && wifi_backend_available();
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
    // UI must have highest app-level priority to keep render loop responsive.
    (void)xTaskCreatePinnedToCore(ui_task, "ui", UI_TASK_STACK_BYTES, nullptr, 14, &s_ui_task, 1);

    // Control loop task (safety-critical: keep running even if UI stalls).
    // Keep it off the UI core to prevent contention with Slint render/event loop.
    if (!ui_only_diag) {
        (void)xTaskCreatePinnedToCore(control_task, "ctrl", 4096, nullptr, 11, nullptr, 0);
    }

    // Networking + periodic WS broadcast task.
    if (network_enabled) {
        (void)xTaskCreatePinnedToCore(server_task, "srv", 6144, nullptr, 6, nullptr, 0);
    }

    ESP_LOGI(TAG, "Main task done; deleting main task to avoid stack overflow.");
    vTaskDelete(NULL);
}
