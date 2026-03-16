#include "ui/slint_bridge.h"

#include "drivers/display_driver.h"
#include "kiln_control/thermal_control.h"

#include <cstring>

extern "C" void slint_bridge_display_init(void) {
    display_driver_init();
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

