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

    std::snprintf(out, out_len, "%02d:%02d", local_tm.tm_hour, local_tm.tm_min);
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
