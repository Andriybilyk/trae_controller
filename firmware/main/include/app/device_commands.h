#pragma once

#include <cstdint>
#include <string>

namespace device_commands {

enum class ResultCode {
    Ok = 0,
    InvalidPayload,
    InvalidSchedule,
    NoSchedule,
    SensorInvalid,
    TouchNotCalibrated,
    FaultActive,
    FanUnsafe,
    ScheduleOverMaxTemp,
    ClockNotSet,
};

struct CommandResult {
    ResultCode code;
    bool ok() const { return code == ResultCode::Ok; }
};

struct FanConfig {
    bool manual;
    bool auto_enabled;
    uint8_t power;
    float temp_min_c;
    float temp_max_c;
    uint8_t power_min;
    uint8_t power_max;
};

CommandResult start_loaded_schedule();
CommandResult start_from_schedule_json(const std::string &schedule_json);
CommandResult start_from_api_payload(const std::string &payload_json);
CommandResult load_schedule_json(const std::string &schedule_json);
CommandResult stop(const char *reason);
CommandResult skip();
CommandResult add_temp(double delta_c);
CommandResult add_time(double delta_minutes);
CommandResult set_rate(double rate_c_per_min);

FanConfig current_fan_config();
void apply_fan_config(const FanConfig &cfg);
void set_fan_manual(bool enabled);
void set_fan_power(int32_t percent);

} // namespace device_commands
