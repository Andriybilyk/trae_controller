#include "ui/slint_bridge.h"

#include "app/device_commands.h"
#include "config/board_profile.h"
#include "drivers/display_driver.h"
#include "drivers/pzem004t_driver.h"
#include "kiln_control/thermal_control.h"
#include "kiln_hal_heater/heater_hal.h"
#include "kiln_config/config_store.h"
#include "drivers/rtc_ds3231.h"
#include "net/wifi_connection.h"
#include "net/wifi_server.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/temperature_sensor.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <time.h>
#include <sys/time.h>

static std::atomic<uint64_t> s_ui_heartbeat_ms{0};
static const char *TAG = "SLINT_BRIDGE";
static constexpr const char *UI_PREF_NS = "ui_pref";
static constexpr const char *UI_PREF_LANG_KEY = "lang";
static constexpr const char *UI_PREF_TEMP_UNIT_KEY = "temp_unit";
static constexpr const char *UI_PREF_TIME_FMT_KEY = "time_fmt";
static constexpr const char *UI_PREF_DATE_FMT_KEY = "date_fmt";
static constexpr const char *UI_PREF_TOUCH_RESTORE_V1 = "touch_restore_v1";
static constexpr uint8_t TEMP_UNIT_C = 0;
static constexpr uint8_t TEMP_UNIT_F = 1;
static constexpr uint8_t TIME_FMT_24H = 0;
static constexpr uint8_t TIME_FMT_12H = 1;
static constexpr uint8_t DATE_FMT_DMY = 0;
static constexpr uint8_t DATE_FMT_MDY = 1;
static constexpr uint8_t DATE_FMT_YMD = 2;
static std::atomic<bool> s_fault_clear_inflight{false};
static std::atomic<uint64_t> s_last_fault_clear_ms{0};
static std::atomic<bool> s_wifi_scan_start_inflight{false};
static std::atomic<uint64_t> s_last_start_cmd_ms{0};
static std::atomic<uint64_t> s_last_stop_cmd_ms{0};
static temperature_sensor_handle_t s_chip_temp_sensor = nullptr;
static bool s_chip_temp_ready = false;
static bool s_chip_temp_init_attempted = false;

static bool notify_slint_command(const char *action,
                                 const device_commands::CommandResult &res,
                                 const char *ok_message);

static bool ensure_chip_temp_sensor_ready() {
    if (s_chip_temp_ready && s_chip_temp_sensor != nullptr) {
        return true;
    }
    if (s_chip_temp_init_attempted) {
        return false;
    }
    s_chip_temp_init_attempted = true;

    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 105);
    if (temperature_sensor_install(&cfg, &s_chip_temp_sensor) != ESP_OK || s_chip_temp_sensor == nullptr) {
        s_chip_temp_sensor = nullptr;
        return false;
    }
    if (temperature_sensor_enable(s_chip_temp_sensor) != ESP_OK) {
        (void)temperature_sensor_uninstall(s_chip_temp_sensor);
        s_chip_temp_sensor = nullptr;
        return false;
    }
    s_chip_temp_ready = true;
    return true;
}

static bool copy_cstr_out(char *out, int32_t out_len, const char *src) {
    if (!out || out_len <= 0 || !src) return false;
    const size_t n = strnlen(src, static_cast<size_t>(out_len - 1));
    memcpy(out, src, n);
    out[n] = '\0';
    return true;
}

#if CONFIG_IDF_TARGET_ESP32P4
struct UiPrefReq {
    char key[16];
    uint8_t val;
};
static QueueHandle_t s_ui_pref_queue = nullptr;
static constexpr TickType_t kUiPrefDebounceTicks = pdMS_TO_TICKS(1200);

static void ui_pref_save_worker(void *) {
    UiPrefReq req;
    while (xQueueReceive(s_ui_pref_queue, &req, portMAX_DELAY) == pdTRUE) {
        UiPrefReq pending = req;
        // Coalesce preference writes and commit once after a short quiet period.
        while (xQueueReceive(s_ui_pref_queue, &req, kUiPrefDebounceTicks) == pdTRUE) {
            pending = req;
        }
        nvs_handle_t h = 0;
        if (nvs_open(UI_PREF_NS, NVS_READWRITE, &h) == ESP_OK) {
            (void)nvs_set_u8(h, pending.key, pending.val);
            (void)nvs_commit(h);
            nvs_close(h);
        }
    }
}

static void ensure_ui_pref_worker() {
    if (s_ui_pref_queue) return;
    s_ui_pref_queue = xQueueCreate(16, sizeof(UiPrefReq));
    if (s_ui_pref_queue) {
        xTaskCreate(ui_pref_save_worker, "ui_pref_save", 3072, nullptr, 1, nullptr);
    }
}
#endif

static uint8_t ui_pref_get_u8(const char *key, uint8_t fallback) {
    nvs_handle_t h = 0;
    if (nvs_open(UI_PREF_NS, NVS_READONLY, &h) != ESP_OK) {
        return fallback;
    }
    uint8_t value = fallback;
    const esp_err_t err = nvs_get_u8(h, key, &value);
    nvs_close(h);
    if (err != ESP_OK) return fallback;
    return value;
}

static void ui_pref_set_u8(const char *key, uint8_t value) {
#if CONFIG_IDF_TARGET_ESP32P4
    ensure_ui_pref_worker();
    if (s_ui_pref_queue) {
        UiPrefReq req{};
        strncpy(req.key, key ? key : "", sizeof(req.key) - 1);
        req.val = value;
        (void)xQueueSend(s_ui_pref_queue, &req, 0);
    }
#else
    nvs_handle_t h = 0;
    if (nvs_open(UI_PREF_NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u8(h, key, value);
    (void)nvs_commit(h);
    nvs_close(h);
#endif
}

static void restore_touch_from_backup_once() {
    if (ui_pref_get_u8(UI_PREF_TOUCH_RESTORE_V1, 0) != 0) {
        return;
    }

    nvs_handle_t h = 0;
    if (nvs_open("touch", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }

    // Values extracted from backup_full_com5.bin (previous firmware).
    (void)nvs_set_u8(h, "mode", 0);
    (void)nvs_set_u32(h, "hz", 250000);
    (void)nvs_set_u8(h, "swap", 1);
    (void)nvs_set_u8(h, "mx", 0);
    (void)nvs_set_u8(h, "my", 0);

    (void)nvs_set_u8(h, "cal_en", 1);
    (void)nvs_set_u16(h, "cal_l", 233);
    (void)nvs_set_u16(h, "cal_r", 3970);
    (void)nvs_set_u16(h, "cal_t", 150);
    (void)nvs_set_u16(h, "cal_b", 3920);

    (void)nvs_set_u8(h, "aff_en", 0);
    const float aff_a = 1.0f;
    const float aff_b = 0.0f;
    const float aff_c = 0.0f;
    const float aff_d = 0.0f;
    const float aff_e = 1.0f;
    const float aff_f = 0.0f;
    (void)nvs_set_blob(h, "aff_a", &aff_a, sizeof(float));
    (void)nvs_set_blob(h, "aff_b", &aff_b, sizeof(float));
    (void)nvs_set_blob(h, "aff_c", &aff_c, sizeof(float));
    (void)nvs_set_blob(h, "aff_d", &aff_d, sizeof(float));
    (void)nvs_set_blob(h, "aff_e", &aff_e, sizeof(float));
    (void)nvs_set_blob(h, "aff_f", &aff_f, sizeof(float));

    (void)nvs_set_u8(h, "grid_en", 0);
    const float grid_zero[9] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    (void)nvs_set_blob(h, "grid_dx", grid_zero, sizeof(grid_zero));
    (void)nvs_set_blob(h, "grid_dy", grid_zero, sizeof(grid_zero));

    (void)nvs_set_i32(h, "sclk", 12);
    (void)nvs_set_i32(h, "mosi", 11);
    (void)nvs_set_i32(h, "miso", 13);

    (void)nvs_commit(h);
    nvs_close(h);

    ui_pref_set_u8(UI_PREF_TOUCH_RESTORE_V1, 1);
}

static void fault_clear_worker_task(void *) {
    const bool cleared = thermalCtrl.clearFault();
    if (cleared) {
        wifiServer.notifyCommandResult("fault_clear", {device_commands::ResultCode::Ok}, "cleared", "slint");
    } else {
        wifiServer.notifyCommandResult("fault_clear", {device_commands::ResultCode::InvalidPayload}, "clear_rejected", "slint");
    }
    s_fault_clear_inflight.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

static void wifi_scan_start_worker_task(void *) {
    ESP_LOGI(TAG, "Wi-Fi scan start worker: begin");
    wifi_start_scan();
    ESP_LOGI(TAG, "Wi-Fi scan start worker: done");
    s_wifi_scan_start_inflight.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

static bool notify_slint_command(const char *action,
                                 const device_commands::CommandResult &res,
                                 const char *ok_message) {
    ESP_LOGI(TAG, "CMD action=%s result=%d ok=%d", action ? action : "?", (int)res.code, res.ok() ? 1 : 0);
    wifiServer.notifyCommandResult(action, res, ok_message, "slint");
    return res.ok();
}

static cJSON *load_settings_root() {
    cJSON *root = nullptr;
    const std::string existing = kiln_config_load_json_config();
    if (!existing.empty()) {
        root = cJSON_Parse(existing.c_str());
    }
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    return root;
}

static bool save_settings_root(cJSON *root) {
    if (!root) return false;
    char *rendered = cJSON_PrintUnformatted(root);
    const std::string out = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    const bool ok = kiln_config_save_json_config(out);
    if (ok) {
        wifiServer.notifySettingsChanged("save");
    }
    return ok;
}

static double read_settings_number(const char *key, double fallback) {
    if (!key) return fallback;
    cJSON *root = load_settings_root();
    if (!root) return fallback;
    const cJSON *item = cJSON_GetObjectItem(root, key);
    const double out = cJSON_IsNumber(item) ? item->valuedouble : fallback;
    cJSON_Delete(root);
    return out;
}

extern "C" void slint_bridge_display_init(void) {
    restore_touch_from_backup_once();
    display_driver_init();
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        // Enable relay GPIO control only after display stack finished initialization.
        heater_hal_set_display_ready(true);
    }
    pzem004t_init();
    s_ui_heartbeat_ms.store((uint64_t)(esp_timer_get_time() / 1000ULL), std::memory_order_relaxed);
}

extern "C" void slint_bridge_get_state(slint_kiln_state_t *out) {
    if (!out) return;
    const KilnState st = thermalCtrl.getState();
    const SafetyStats safety = thermalCtrl.getSafetyStats();
    pzem004t_reading_t reading = {};
    const bool pzem_ok = pzem004t_get_reading(&reading);

    out->current_temp = st.currentTemp;
    out->target_temp = st.targetTemp;
    out->status = static_cast<int32_t>(st.status);
    out->is_firing = st.isFiring;
    out->time_remaining = st.timeRemaining;
    out->current_step = st.currentStep;
    out->total_steps = st.totalSteps;
    out->fault_active = safety.faultActive;

    std::memset(out->error_msg, 0, sizeof(out->error_msg));
    std::memset(out->fault_reason, 0, sizeof(out->fault_reason));

    if (!st.errorMsg.empty()) {
        std::strncpy(out->error_msg, st.errorMsg.c_str(), sizeof(out->error_msg) - 1);
    }
    if (safety.faultReason[0]) {
        std::strncpy(out->fault_reason, safety.faultReason, sizeof(out->fault_reason) - 1);
    }
    out->pzem_voltage = reading.voltage;
    out->pzem_current = reading.current;
    out->pzem_power = reading.power;
    out->pzem_ok = pzem_ok;
}

extern "C" bool slint_bridge_start_schedule_json(const char *json) {
    if (!json || !json[0]) return false;
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    const uint64_t prev_start = s_last_start_cmd_ms.load(std::memory_order_acquire);
    if (prev_start != 0 && (now_ms - prev_start) < 800ULL) {
        ESP_LOGW(TAG, "START ignored: cooldown (%llu ms since previous)", (unsigned long long)(now_ms - prev_start));
        return false;
    }
    s_last_start_cmd_ms.store(now_ms, std::memory_order_release);
    return notify_slint_command("start",
                                device_commands::start_from_schedule_json(std::string(json)),
                                "started");
}

extern "C" bool slint_bridge_load_schedule_json(const char *json) {
    if (!json || !json[0]) return false;
    return notify_slint_command("load_schedule",
                                device_commands::load_schedule_json(std::string(json)),
                                "loaded");
}

extern "C" void slint_bridge_stop(void) {
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    const uint64_t prev_stop = s_last_stop_cmd_ms.load(std::memory_order_acquire);
    if (prev_stop != 0 && (now_ms - prev_stop) < 800ULL) {
        ESP_LOGW(TAG, "STOP ignored: cooldown (%llu ms since previous)", (unsigned long long)(now_ms - prev_stop));
        return;
    }
    s_last_stop_cmd_ms.store(now_ms, std::memory_order_release);
    (void)notify_slint_command("stop", device_commands::stop("User Button"), "stopped");
}

extern "C" void slint_bridge_skip_step(void) {
    (void)notify_slint_command("skip", device_commands::skip(), "ok");
}

extern "C" void slint_bridge_add_temp(float delta_c) {
    (void)notify_slint_command("add_temp", device_commands::add_temp((double)delta_c), "ok");
}

extern "C" void slint_bridge_add_time(float delta_minutes) {
    (void)notify_slint_command("add_time", device_commands::add_time((double)delta_minutes), "ok");
}

extern "C" bool slint_bridge_set_rate(float rate_c_per_min) {
    return notify_slint_command("set_rate", device_commands::set_rate((double)rate_c_per_min), "ok");
}

extern "C" bool slint_bridge_start_autotune(float target_c) {
    const bool ok = thermalCtrl.startAutotune(target_c);
    if (ok) {
        wifiServer.notifyAutotuneState("start");
        wifiServer.notifyCommandResult("autotune_start", {device_commands::ResultCode::Ok}, "started", "slint");
    } else {
        wifiServer.notifyCommandResult("autotune_start", {device_commands::ResultCode::SensorInvalid}, "started", "slint");
    }
    return ok;
}

extern "C" void slint_bridge_stop_autotune(void) {
    thermalCtrl.stopAutotune("Autotune stopped");
    wifiServer.notifyAutotuneState("stop");
    wifiServer.notifyCommandResult("autotune_stop", {device_commands::ResultCode::Ok}, "stopped", "slint");
}

extern "C" float slint_bridge_get_user_max_temp_c(void) {
    return thermalCtrl.getUserMaxTemperatureC();
}

extern "C" bool slint_bridge_set_user_max_temp_c(float max_c) {
    if (!std::isfinite(max_c)) return false;
    const float clamped = std::clamp(max_c, 100.0f, 1300.0f);
    if (!thermalCtrl.setUserMaxTemperatureC(clamped)) {
        return false;
    }
    cJSON *root = load_settings_root();
    if (!root) return false;
    cJSON_DeleteItemFromObject(root, "maxC");
    cJSON_AddNumberToObject(root, "maxC", (double)clamped);
    const bool ok = save_settings_root(root);
    cJSON_Delete(root);
    return ok;
}

extern "C" float slint_bridge_get_temperature_offset_c(void) {
    return thermalCtrl.getTemperatureOffset();
}

extern "C" bool slint_bridge_set_temperature_offset_c(float offset_c) {
    if (!std::isfinite(offset_c)) return false;
    const float clamped = std::clamp(offset_c, -100.0f, 100.0f);
    if (!thermalCtrl.setTemperatureOffset(clamped)) {
        return false;
    }
    cJSON *root = load_settings_root();
    if (!root) return false;
    cJSON_DeleteItemFromObject(root, "temp_offset_c");
    cJSON_AddNumberToObject(root, "temp_offset_c", (double)clamped);
    cJSON_DeleteItemFromObject(root, "offset");
    cJSON_AddNumberToObject(root, "offset", (double)clamped);
    const bool ok = save_settings_root(root);
    cJSON_Delete(root);
    return ok;
}

extern "C" int32_t slint_bridge_get_kiln_wattage(void) {
    const double watts = read_settings_number("wattage", 0.0);
    if (!std::isfinite(watts)) return 0;
    return (int32_t)std::lround(watts);
}

extern "C" bool slint_bridge_set_kiln_wattage(int32_t watts) {
    const int32_t clamped = std::clamp<int32_t>(watts, 100, 50000);
    cJSON *root = load_settings_root();
    if (!root) return false;
    cJSON_DeleteItemFromObject(root, "wattage");
    cJSON_AddNumberToObject(root, "wattage", (double)clamped);
    const bool ok = save_settings_root(root);
    cJSON_Delete(root);
    return ok;
}

extern "C" float slint_bridge_get_autotune_target_c(void) {
    return thermalCtrl.getAutotuneTargetC();
}

extern "C" bool slint_bridge_set_autotune_target_c(float target_c) {
    return thermalCtrl.setAutotuneTargetC(target_c);
}

extern "C" bool slint_bridge_clear_fault(void) {
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    const uint64_t last_ms = s_last_fault_clear_ms.load(std::memory_order_relaxed);
    if ((now_ms > last_ms) && (now_ms - last_ms < 800)) {
        wifiServer.notifyCommandResult("fault_clear", {device_commands::ResultCode::InvalidPayload}, "busy", "slint");
        return false;
    }
    bool expected = false;
    if (!s_fault_clear_inflight.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        wifiServer.notifyCommandResult("fault_clear", {device_commands::ResultCode::InvalidPayload}, "busy", "slint");
        return false;
    }
    s_last_fault_clear_ms.store(now_ms, std::memory_order_relaxed);
    const BaseType_t task_ok = xTaskCreate(
        fault_clear_worker_task,
        "fault_clear",
        4096,
        nullptr,
        tskIDLE_PRIORITY + 1,
        nullptr
    );
    if (task_ok != pdPASS) {
        s_fault_clear_inflight.store(false, std::memory_order_release);
        wifiServer.notifyCommandResult("fault_clear", {device_commands::ResultCode::InvalidPayload}, "busy", "slint");
        return false;
    }
    return true;
}

extern "C" void slint_bridge_set_fan_manual(bool enabled) {
    device_commands::set_fan_manual(enabled);
    (void)notify_slint_command("fan_manual", {device_commands::ResultCode::Ok}, "ok");
}

extern "C" void slint_bridge_set_fan_power(int32_t percent) {
    device_commands::set_fan_power(percent);
    (void)notify_slint_command("fan_power", {device_commands::ResultCode::Ok}, "ok");
}

extern "C" bool slint_bridge_wifi_is_connected(void) {
    return wifi_is_connected();
}

extern "C" bool slint_bridge_wifi_connect(const char *ssid, const char *password) {
    return wifi_connect_with_credentials(ssid, password);
}

extern "C" void slint_bridge_wifi_disconnect(void) {
    wifi_disconnect_and_forget();
}

extern "C" void slint_bridge_wifi_scan_start(void) {
    bool expected = false;
    if (!s_wifi_scan_start_inflight.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ESP_LOGW(TAG, "Wi-Fi scan start ignored: worker already in-flight");
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi scan start requested from UI (async)");
    const BaseType_t task_ok = xTaskCreate(
        wifi_scan_start_worker_task,
        "wifi_scan_start",
        4096,
        nullptr,
        tskIDLE_PRIORITY + 1,
        nullptr
    );
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create wifi_scan_start worker task");
        s_wifi_scan_start_inflight.store(false, std::memory_order_release);
    }
}

extern "C" bool slint_bridge_wifi_scan_ready(void) {
    return wifi_is_scan_done();
}

extern "C" bool slint_bridge_wifi_scan_copy_results(char *out, int32_t out_len) {
    if (!out || out_len <= 0) return false;
    const std::string json = wifi_get_scanned_networks();
    const std::string payload = json.empty() ? "[]" : json;
    const size_t n = std::min((size_t)(out_len - 1), payload.size());
    std::memcpy(out, payload.data(), n);
    out[n] = '\0';
    return true;
}

extern "C" bool slint_bridge_wifi_server_url_copy(char *out, int32_t out_len) {
    if (!out || out_len <= 0) return false;
    const std::string url = wifi_get_server_url();
    const size_t n = std::min((size_t)(out_len - 1), url.size());
    std::memcpy(out, url.data(), n);
    out[n] = '\0';
    return true;
}

extern "C" bool slint_bridge_wifi_ap_ssid_copy(char *out, int32_t out_len) {
    if (!out || out_len <= 0) return false;
    const std::string ssid = wifi_get_ap_ssid();
    const size_t n = std::min((size_t)(out_len - 1), ssid.size());
    std::memcpy(out, ssid.data(), n);
    out[n] = '\0';
    return true;
}

extern "C" uint64_t slint_bridge_wifi_last_got_ip_ms(void) {
    return wifi_last_sta_got_ip_ms();
}

extern "C" uint32_t slint_bridge_get_schedules_revision(void) {
    return wifiServer.getSchedulesRevision();
}

extern "C" uint32_t slint_bridge_get_command_result_revision(void) {
    return wifiServer.getCommandResultRevision();
}

extern "C" bool slint_bridge_copy_command_result(slint_command_result_t *out) {
    if (!out) return false;
    command_result_snapshot_t snap{};
    if (!wifiServer.copyLastCommandResult(&snap) || !snap.valid) return false;

    out->rev = snap.rev;
    out->ts_ms = snap.ts_ms;
    out->ok = snap.ok;
    std::memset(out->action, 0, sizeof(out->action));
    std::memset(out->source, 0, sizeof(out->source));
    std::memset(out->code, 0, sizeof(out->code));
    std::memset(out->message, 0, sizeof(out->message));
    std::strncpy(out->action, snap.action, sizeof(out->action) - 1);
    std::strncpy(out->source, snap.source, sizeof(out->source) - 1);
    std::strncpy(out->code, snap.code, sizeof(out->code) - 1);
    std::strncpy(out->message, snap.message, sizeof(out->message) - 1);
    return true;
}

extern "C" void slint_bridge_notify_schedules_changed(void) {
    wifiServer.notifySchedulesChanged("ui_save", nullptr);
}

extern "C" void slint_bridge_ui_heartbeat(void) {
    s_ui_heartbeat_ms.store((uint64_t)(esp_timer_get_time() / 1000ULL), std::memory_order_relaxed);
}

extern "C" uint64_t slint_bridge_get_ui_heartbeat_ms(void) {
    return s_ui_heartbeat_ms.load(std::memory_order_relaxed);
}

extern "C" bool slint_bridge_get_time_str(char *out, int32_t out_len) {
    if (!out || out_len <= 0) return false;
    out[0] = '\0';

    time_t now = time(nullptr);
    struct tm local_tm;
    if (now < 100000 || !localtime_r(&now, &local_tm)) {
        std::snprintf(out, out_len, "--:--");
        return false;
    }
    const int year = local_tm.tm_year + 1900;
    if (year < 2020) {
        std::snprintf(out, out_len, "--:--");
        return false;
    }

    const uint8_t fmt = ui_pref_get_u8(UI_PREF_TIME_FMT_KEY, TIME_FMT_24H);
    if (fmt == TIME_FMT_12H) {
        int hour = local_tm.tm_hour % 12;
        if (hour == 0) hour = 12;
        const bool is_pm = local_tm.tm_hour >= 12;
        std::snprintf(out, out_len, "%02d:%02d %s", hour, local_tm.tm_min, is_pm ? "PM" : "AM");
    } else {
        std::snprintf(out, out_len, "%02d:%02d", local_tm.tm_hour, local_tm.tm_min);
    }
    return true;
}

extern "C" bool slint_bridge_get_date_str(char *out, int32_t out_len) {
    if (!out || out_len <= 0) return false;
    out[0] = '\0';

    time_t now = time(nullptr);
    struct tm local_tm;
    if (now < 100000 || !localtime_r(&now, &local_tm)) {
        std::snprintf(out, out_len, "--.--.----");
        return false;
    }
    const int year = local_tm.tm_year + 1900;
    if (year < 2020) {
        std::snprintf(out, out_len, "--.--.----");
        return false;
    }

    const uint8_t fmt = ui_pref_get_u8(UI_PREF_DATE_FMT_KEY, DATE_FMT_DMY);
    if (fmt == DATE_FMT_MDY) {
        std::snprintf(out, out_len, "%02d/%02d/%04d", local_tm.tm_mon + 1, local_tm.tm_mday, year);
    } else if (fmt == DATE_FMT_YMD) {
        std::snprintf(out, out_len, "%04d-%02d-%02d", year, local_tm.tm_mon + 1, local_tm.tm_mday);
    } else {
        std::snprintf(out, out_len, "%02d.%02d.%04d", local_tm.tm_mday, local_tm.tm_mon + 1, year);
    }
    return true;
}

extern "C" bool slint_bridge_get_firmware_version_copy(char *out, int32_t out_len) {
    const esp_app_desc_t *desc = esp_app_get_description();
    if (!desc || !desc->version[0]) return copy_cstr_out(out, out_len, "unknown");
    return copy_cstr_out(out, out_len, desc->version);
}

extern "C" bool slint_bridge_get_device_id_copy(char *out, int32_t out_len) {
    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        return copy_cstr_out(out, out_len, "--");
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return copy_cstr_out(out, out_len, buf);
}

extern "C" uint64_t slint_bridge_get_uptime_seconds(void) {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
}

extern "C" uint32_t slint_bridge_get_free_heap_bytes(void) {
    return static_cast<uint32_t>(esp_get_free_heap_size());
}

extern "C" uint32_t slint_bridge_get_total_heap_bytes(void) {
    return static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_8BIT));
}

extern "C" uint32_t slint_bridge_get_free_psram_bytes(void) {
    return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

extern "C" uint32_t slint_bridge_get_total_psram_bytes(void) {
    return static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
}

extern "C" bool slint_bridge_get_chip_temp_c(float *out_c) {
    if (!out_c) return false;
    if (!ensure_chip_temp_sensor_ready()) return false;
    float temp_c = 0.0f;
    if (temperature_sensor_get_celsius(s_chip_temp_sensor, &temp_c) != ESP_OK) {
        return false;
    }
    *out_c = temp_c;
    return true;
}

extern "C" uint8_t slint_bridge_get_board_profile(void) {
    switch (board_profile::current_board()) {
        case board_profile::BoardId::NewP4:
            return 1;
        case board_profile::BoardId::LegacyS3:
        default:
            return 0;
    }
}

extern "C" int32_t slint_bridge_get_display_width(void) {
    return board_profile::display_width();
}

extern "C" int32_t slint_bridge_get_display_height(void) {
    return board_profile::display_height();
}

static bool load_current_tm(struct tm *out_tm) {
    if (!out_tm) return false;
    time_t now = time(nullptr);
    if (now >= 1577836800 && localtime_r(&now, out_tm)) {
        out_tm->tm_isdst = -1;
        return true;
    }
    bool rtc_valid = false;
    if (rtc_ds3231_read_time(out_tm, &rtc_valid) && rtc_valid) {
        out_tm->tm_isdst = -1;
        return true;
    }
    std::memset(out_tm, 0, sizeof(*out_tm));
    out_tm->tm_year = 124;
    out_tm->tm_mon = 0;
    out_tm->tm_mday = 1;
    out_tm->tm_isdst = -1;
    return false;
}

extern "C" bool slint_bridge_set_time_hm(uint8_t hour, uint8_t minute) {
    if (hour > 23 || minute > 59) return false;
    struct tm tmv = {};
    (void)load_current_tm(&tmv);
    tmv.tm_hour = hour;
    tmv.tm_min = minute;
    tmv.tm_sec = 0;
    if (!rtc_ds3231_set_time(&tmv)) return false;
    const time_t ts = mktime(&tmv);
    if (ts >= 1577836800) {
        struct timeval tv = {};
        tv.tv_sec = ts;
        tv.tv_usec = 0;
        (void)settimeofday(&tv, nullptr);
    }
    return true;
}

extern "C" bool slint_bridge_set_date_ymd(uint16_t year, uint8_t month, uint8_t day) {
    if (year < 2020 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31) return false;
    struct tm tmv = {};
    (void)load_current_tm(&tmv);
    tmv.tm_year = (int)year - 1900;
    tmv.tm_mon = (int)month - 1;
    tmv.tm_mday = (int)day;
    tmv.tm_sec = tmv.tm_sec < 0 ? 0 : tmv.tm_sec;
    if (!rtc_ds3231_set_time(&tmv)) return false;
    const time_t ts = mktime(&tmv);
    if (ts >= 1577836800) {
        struct timeval tv = {};
        tv.tv_sec = ts;
        tv.tv_usec = 0;
        (void)settimeofday(&tv, nullptr);
    }
    return true;
}

extern "C" bool slint_bridge_get_language_is_ua(void) {
    nvs_handle_t h = 0;
    if (nvs_open(UI_PREF_NS, NVS_READONLY, &h) != ESP_OK) {
        return true; // default UA for first boot
    }
    uint8_t value = 1;
    esp_err_t err = nvs_get_u8(h, UI_PREF_LANG_KEY, &value);
    nvs_close(h);
    if (err != ESP_OK) return true;
    return value != 0;
}

extern "C" void slint_bridge_set_language_is_ua(bool is_ua) {
    ui_pref_set_u8(UI_PREF_LANG_KEY, is_ua ? 1 : 0);
}

extern "C" uint8_t slint_bridge_get_temp_unit(void) {
    const uint8_t v = ui_pref_get_u8(UI_PREF_TEMP_UNIT_KEY, TEMP_UNIT_C);
    return (v == TEMP_UNIT_F) ? TEMP_UNIT_F : TEMP_UNIT_C;
}

extern "C" void slint_bridge_set_temp_unit(uint8_t unit) {
    const uint8_t v = (unit == TEMP_UNIT_F) ? TEMP_UNIT_F : TEMP_UNIT_C;
    ui_pref_set_u8(UI_PREF_TEMP_UNIT_KEY, v);
}

extern "C" uint8_t slint_bridge_get_time_format(void) {
    const uint8_t v = ui_pref_get_u8(UI_PREF_TIME_FMT_KEY, TIME_FMT_24H);
    return (v == TIME_FMT_12H) ? TIME_FMT_12H : TIME_FMT_24H;
}

extern "C" void slint_bridge_set_time_format(uint8_t fmt) {
    const uint8_t v = (fmt == TIME_FMT_12H) ? TIME_FMT_12H : TIME_FMT_24H;
    ui_pref_set_u8(UI_PREF_TIME_FMT_KEY, v);
}

extern "C" uint8_t slint_bridge_get_date_format(void) {
    const uint8_t v = ui_pref_get_u8(UI_PREF_DATE_FMT_KEY, DATE_FMT_DMY);
    if (v == DATE_FMT_MDY || v == DATE_FMT_YMD) return v;
    return DATE_FMT_DMY;
}

extern "C" void slint_bridge_set_date_format(uint8_t fmt) {
    uint8_t v = DATE_FMT_DMY;
    if (fmt == DATE_FMT_MDY) v = DATE_FMT_MDY;
    if (fmt == DATE_FMT_YMD) v = DATE_FMT_YMD;
    ui_pref_set_u8(UI_PREF_DATE_FMT_KEY, v);
}

extern "C" uint8_t slint_bridge_get_display_brightness(void) {
    return display_driver_get_backlight_percent();
}

extern "C" void slint_bridge_set_display_brightness(uint8_t percent) {
    ESP_LOGI(TAG, "UI brightness set request: %u", (unsigned)percent);
    display_driver_set_backlight_percent(percent);
}
