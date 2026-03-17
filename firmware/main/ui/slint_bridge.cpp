#include "ui/slint_bridge.h"

#include "drivers/display_driver.h"
#include "drivers/fan_driver.h"
#include "kiln_control/thermal_control.h"
#include "net/wifi_connection.h"
#include "net/wifi_server.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <algorithm>
#include <cstring>
#include <string>

static std::atomic<uint64_t> s_ui_heartbeat_ms{0};

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

    if (!thermalCtrl.isSensorHealthy()) {
        thermalCtrl.stop("Start blocked: sensor invalid");
        return false;
    }
    if (!display_driver_is_touch_calibrated()) {
        thermalCtrl.stop("Start blocked: touch calibration required");
        return false;
    }

    thermalCtrl.loadSchedule(std::string(json));
    thermalCtrl.start();
    return true;
}

extern "C" bool slint_bridge_load_schedule_json(const char *json) {
    if (!json || !json[0]) return false;
    thermalCtrl.loadSchedule(std::string(json));
    return true;
}

extern "C" void slint_bridge_stop(void) {
    thermalCtrl.stop("User Button");
}

extern "C" void slint_bridge_set_fan_manual(bool enabled) {
    fan_driver_set_manual(enabled);
}

extern "C" void slint_bridge_set_fan_power(int32_t percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    fan_driver_set_power_percent((uint8_t)percent);
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

extern "C" uint32_t slint_bridge_get_schedules_revision(void) {
    return wifiServer.getSchedulesRevision();
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

