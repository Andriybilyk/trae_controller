#include "ui/slint_bridge.h"

#include "app/device_commands.h"
#include "drivers/display_driver.h"
#include "kiln_control/thermal_control.h"
#include "net/wifi_connection.h"
#include "net/wifi_server.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <time.h>

static std::atomic<uint64_t> s_ui_heartbeat_ms{0};
static constexpr const char *UI_PREF_NS = "ui_pref";
static constexpr const char *UI_PREF_LANG_KEY = "lang";
static constexpr const char *UI_PREF_TEMP_UNIT_KEY = "temp_unit";
static constexpr const char *UI_PREF_TIME_FMT_KEY = "time_fmt";
static constexpr const char *UI_PREF_DATE_FMT_KEY = "date_fmt";
static constexpr uint8_t TEMP_UNIT_C = 0;
static constexpr uint8_t TEMP_UNIT_F = 1;
static constexpr uint8_t TIME_FMT_24H = 0;
static constexpr uint8_t TIME_FMT_12H = 1;
static constexpr uint8_t DATE_FMT_DMY = 0;
static constexpr uint8_t DATE_FMT_MDY = 1;
static constexpr uint8_t DATE_FMT_YMD = 2;
static std::atomic<bool> s_fault_clear_inflight{false};
static std::atomic<uint64_t> s_last_fault_clear_ms{0};

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
    nvs_handle_t h = 0;
    if (nvs_open(UI_PREF_NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u8(h, key, value);
    (void)nvs_commit(h);
    nvs_close(h);
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

static bool notify_slint_command(const char *action,
                                 const device_commands::CommandResult &res,
                                 const char *ok_message) {
    wifiServer.notifyCommandResult(action, res, ok_message, "slint");
    return res.ok();
}

extern "C" void slint_bridge_display_init(void) {
    display_driver_init();
    s_ui_heartbeat_ms.store((uint64_t)(esp_timer_get_time() / 1000ULL), std::memory_order_relaxed);
}

extern "C" void slint_bridge_get_state(slint_kiln_state_t *out) {
    if (!out) return;
    const KilnState st = thermalCtrl.getState();
    const SafetyStats safety = thermalCtrl.getSafetyStats();

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
}

extern "C" bool slint_bridge_start_schedule_json(const char *json) {
    if (!json || !json[0]) return false;
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
    if (!wifi_connect_with_credentials(ssid, password)) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return true;
}

extern "C" void slint_bridge_wifi_disconnect(void) {
    wifi_disconnect_and_forget();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

extern "C" void slint_bridge_wifi_scan_start(void) {
    wifi_start_scan();
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
    (void)esp_task_wdt_reset();
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
    nvs_handle_t h = 0;
    if (nvs_open(UI_PREF_NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u8(h, UI_PREF_LANG_KEY, is_ua ? 1 : 0);
    (void)nvs_commit(h);
    nvs_close(h);
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
