#include "app/device_commands.h"

#include "drivers/display_driver.h"
#include "drivers/fan_driver.h"
#include "drivers/rtc_ds3231.h"
#include "config/board_profile.h"
#include "kiln_config/config_store.h"
#include "kiln_control/thermal_control.h"

#include "cJSON.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>

namespace device_commands {
static constexpr uint8_t kFanMinOnPercent = 60;

static std::string to_lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static bool default_require_touch_for_mode(const std::string &mode_raw) {
    const std::string mode = to_lower_ascii(mode_raw);
    if (mode == "headless" || mode == "remote") return false;
    return true; // panel/default
}

static bool require_touch_calibration_for_start_policy() {
    const std::string cfg_raw = kiln_config_load_json_config();
    if (cfg_raw.empty()) return true; // panel-like default

    cJSON *root = cJSON_ParseWithLength(cfg_raw.c_str(), cfg_raw.size());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return true;
    }

    std::string mode = "panel";
    const cJSON *mode_item = cJSON_GetObjectItem(root, "deployment_mode");
    if (cJSON_IsString(mode_item) && mode_item->valuestring && mode_item->valuestring[0]) {
        mode = mode_item->valuestring;
    }

    bool require_touch = default_require_touch_for_mode(mode);
    const cJSON *explicit_policy = cJSON_GetObjectItem(root, "require_touch_calibration_for_start");
    if (cJSON_IsBool(explicit_policy)) {
        require_touch = cJSON_IsTrue(explicit_policy);
    }

    cJSON_Delete(root);
    return require_touch;
}

static CommandResult check_start_interlocks() {
    const bool is_new_p4 = (board_profile::current_board() == board_profile::BoardId::NewP4);
    const SafetyStats safety = thermalCtrl.getSafetyStats();
    if (safety.faultActive) {
        return {ResultCode::FaultActive};
    }
    if (!thermalCtrl.isSensorHealthy()) {
        return {ResultCode::SensorInvalid};
    }

    // NewP4 bring-up profile may intentionally skip fan driver init.
    if (!is_new_p4) {
        const FanConfig fan = current_fan_config();
        if ((fan.manual && fan.power == 0) || (!fan.auto_enabled && !fan.manual)) {
            return {ResultCode::FanUnsafe};
        }
    }

    const float maxC = thermalCtrl.getUserMaxTemperatureC();
    const float scheduleMax = thermalCtrl.getLoadedScheduleMaxTargetC();
    if (scheduleMax > 0.0f && scheduleMax > maxC + 0.5f) {
        return {ResultCode::ScheduleOverMaxTemp};
    }

    // NewP4 profile skips DS3231 init in app_main, so don't gate start by RTC validity there.
    if (!is_new_p4) {
        const std::time_t now = std::time(nullptr);
        bool rtc_valid = false;
        (void)rtc_ds3231_is_clock_valid(&rtc_valid);
        if (now < 1704067200 && !rtc_valid) {
            return {ResultCode::ClockNotSet};
        }
    }

    // NewP4 uses GT911 touch path and should not be gated by XPT2046 calibration policy.
    if (!is_new_p4 && require_touch_calibration_for_start_policy() && !display_driver_is_touch_calibrated()) {
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

    CommandResult loaded{ResultCode::InvalidSchedule};
    if (schedule && cJSON_IsObject(schedule)) {
        char *rendered = cJSON_PrintUnformatted(schedule);
        if (rendered) {
            loaded = load_schedule_json(rendered);
            free(rendered);
        }
    } else if (steps) {
        loaded = load_schedule_json(payload_json);
    }
    cJSON_Delete(root);

    if (!loaded.ok()) return loaded;
    return start_loaded_schedule();
}

CommandResult load_schedule_json(const std::string &schedule_json) {
    if (schedule_json.empty()) return {ResultCode::InvalidSchedule};
    if (!thermalCtrl.loadSchedule(schedule_json)) return {ResultCode::InvalidSchedule};
    return {ResultCode::Ok};
}

CommandResult stop(const char *reason) {
    thermalCtrl.stop(reason ? reason : "Stop request");
    return {ResultCode::Ok};
}

CommandResult skip() {
    if (!thermalCtrl.skipCurrentStep()) return {ResultCode::InvalidState};
    return {ResultCode::Ok};
}

CommandResult add_temp(double delta_c) {
    if (!std::isfinite(delta_c)) return {ResultCode::InvalidPayload};
    if (!thermalCtrl.addTemperatureToTarget((float)delta_c)) return {ResultCode::InvalidState};
    return {ResultCode::Ok};
}

CommandResult add_time(double delta_minutes) {
    if (!std::isfinite(delta_minutes)) return {ResultCode::InvalidPayload};
    if (!thermalCtrl.addTimeToHold((float)delta_minutes)) return {ResultCode::InvalidState};
    return {ResultCode::Ok};
}

CommandResult set_rate(double rate_c_per_min) {
    if (!std::isfinite(rate_c_per_min) || rate_c_per_min <= 0.0) return {ResultCode::InvalidPayload};
    if (!thermalCtrl.setCurrentRampRate((float)rate_c_per_min)) return {ResultCode::InvalidState};
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
