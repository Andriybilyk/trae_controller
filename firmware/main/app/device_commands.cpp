#include "app/device_commands.h"

#include "drivers/display_driver.h"
#include "drivers/fan_driver.h"
#include "kiln_control/thermal_control.h"

#include "cJSON.h"

#include <algorithm>
#include <cmath>

namespace device_commands {
static constexpr uint8_t kFanMinOnPercent = 60;

static CommandResult check_start_interlocks() {
    if (!thermalCtrl.isSensorHealthy()) {
        thermalCtrl.stop("Start blocked: sensor invalid");
        return {ResultCode::SensorInvalid};
    }
    if (!display_driver_is_touch_calibrated()) {
        thermalCtrl.stop("Start blocked: touch calibration required");
        return {ResultCode::TouchNotCalibrated};
    }
    return {ResultCode::Ok};
}

CommandResult start_loaded_schedule() {
    if (thermalCtrl.getState().totalSteps <= 0) {
        return {ResultCode::NoSchedule};
    }
    const CommandResult guard = check_start_interlocks();
    if (!guard.ok()) return guard;

    thermalCtrl.start();
    return {ResultCode::Ok};
}

CommandResult start_from_schedule_json(const std::string &schedule_json) {
    const CommandResult loaded = load_schedule_json(schedule_json);
    if (!loaded.ok()) return loaded;
    return start_loaded_schedule();
}

CommandResult start_from_api_payload(const std::string &payload_json) {
    if (payload_json.empty()) return start_loaded_schedule();

    cJSON *root = cJSON_Parse(payload_json.c_str());
    if (!root) return {ResultCode::InvalidPayload};

    cJSON *schedule = cJSON_GetObjectItem(root, "schedule");
    cJSON *steps = cJSON_GetObjectItem(root, "steps");
    if (!steps) steps = cJSON_GetObjectItem(root, "segments");

    bool loaded = false;
    if (schedule && cJSON_IsObject(schedule)) {
        char *rendered = cJSON_PrintUnformatted(schedule);
        if (rendered) {
            thermalCtrl.loadSchedule(rendered);
            free(rendered);
            loaded = true;
        }
    } else if (steps) {
        thermalCtrl.loadSchedule(payload_json);
        loaded = true;
    }
    cJSON_Delete(root);

    if (!loaded) return {ResultCode::InvalidSchedule};
    return start_loaded_schedule();
}

CommandResult load_schedule_json(const std::string &schedule_json) {
    if (schedule_json.empty()) return {ResultCode::InvalidSchedule};
    thermalCtrl.loadSchedule(schedule_json);
    return {ResultCode::Ok};
}

CommandResult stop(const char *reason) {
    thermalCtrl.stop(reason ? reason : "Stop request");
    return {ResultCode::Ok};
}

CommandResult skip() {
    thermalCtrl.skipCurrentStep();
    return {ResultCode::Ok};
}

CommandResult add_temp(double delta_c) {
    if (!std::isfinite(delta_c)) return {ResultCode::InvalidPayload};
    thermalCtrl.addTemperatureToTarget(delta_c);
    return {ResultCode::Ok};
}

CommandResult add_time(double delta_minutes) {
    if (!std::isfinite(delta_minutes)) return {ResultCode::InvalidPayload};
    thermalCtrl.addTimeToHold(delta_minutes);
    return {ResultCode::Ok};
}

FanConfig current_fan_config() {
    FanConfig cfg{};
    cfg.manual = fan_driver_get_manual();
    cfg.auto_enabled = fan_driver_get_auto_enabled();
    cfg.power = fan_driver_get_power_percent();
    cfg.temp_min_c = 45.0f;
    cfg.temp_max_c = 280.0f;
    cfg.power_min = 20;
    cfg.power_max = 100;
    fan_driver_get_auto_curve(&cfg.temp_min_c, &cfg.temp_max_c, &cfg.power_min, &cfg.power_max);
    return cfg;
}

void apply_fan_config(const FanConfig &cfg_in) {
    FanConfig cfg = cfg_in;
    if (cfg.power > 100) cfg.power = 100;
    if (cfg.power_min > 100) cfg.power_min = 100;
    if (cfg.power_max > 100) cfg.power_max = 100;
    if (cfg.power_min < kFanMinOnPercent) cfg.power_min = kFanMinOnPercent;
    if (cfg.power_max < kFanMinOnPercent) cfg.power_max = kFanMinOnPercent;
    if (cfg.power > 0 && cfg.power < kFanMinOnPercent) cfg.power = kFanMinOnPercent;
    if (cfg.power_min > cfg.power_max) std::swap(cfg.power_min, cfg.power_max);
    if (cfg.temp_min_c > cfg.temp_max_c) std::swap(cfg.temp_min_c, cfg.temp_max_c);

    fan_driver_set_auto_curve(cfg.temp_min_c, cfg.temp_max_c, cfg.power_min, cfg.power_max);
    fan_driver_set_auto_enabled(cfg.auto_enabled);
    fan_driver_set_power_percent(cfg.power);
    fan_driver_set_manual(cfg.manual);
}

void set_fan_manual(bool enabled) {
    FanConfig cfg = current_fan_config();
    cfg.manual = enabled;
    apply_fan_config(cfg);
}

void set_fan_power(int32_t percent) {
    FanConfig cfg = current_fan_config();
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent > 0 && percent < kFanMinOnPercent) percent = kFanMinOnPercent;
    cfg.power = static_cast<uint8_t>(percent);
    apply_fan_config(cfg);
}

} // namespace device_commands
