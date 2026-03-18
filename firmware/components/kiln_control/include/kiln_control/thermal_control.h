#ifndef THERMAL_CONTROL_H
#define THERMAL_CONTROL_H

#include <cstdint>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class MAX6675;
class PID;

enum KilnStatus {
    KILN_IDLE = 0,
    KILN_PREHEAT = 1,
    KILN_RAMP = 2,
    KILN_HOLD = 3,
    KILN_COOL = 4,
    KILN_COMPLETE = 5,
    KILN_ERROR = 6,
    KILN_TUNING = 7
};

struct KilnState {
    float currentTemp;
    float targetTemp;
    KilnStatus status;
    bool isFiring;
    std::string errorMsg;
    uint64_t startTime;
    uint32_t duration; // Running duration in seconds
    int timeRemaining; // Remaining time in minutes (-1 if unknown)
    int currentStep;
    int totalSteps;
};

struct ThermoStats {
    uint16_t raw;
    uint32_t readCount;
};

struct SafetyStats {
    bool faultActive;
    uint32_t faultCode;
    uint64_t faultSinceMs;
    char faultReason[96];
};

class ThermalController {
public:
    ThermalController();
    ~ThermalController();
    void begin();
    void loop(); // Call this in a loop or task

    // Commands
    void startSchedule(const std::string& scheduleJson);
    void loadSchedule(const std::string& scheduleJson);
    void start();
    void stop(const std::string& reason = std::string("User Request"));
    bool startAutotune(float setpointC);
    void stopAutotune(const std::string& reason = std::string("User Request"));
    void emergencyStop(const std::string& reason);
    bool clearFault();
    void skipCurrentStep();
    void addTemperatureToTarget(float tempToAdd);
    void addTimeToHold(float minutesToAdd);
    bool setTemperatureOffset(float offsetC);
    float getTemperatureOffset();

    struct AutoTuneStatus {
        bool active;
        bool heaterOn;
        float setpointC;
        int cycles;
        float ku;
        float pu_s;
        double kp;
        double ki;
        double kd;
    };
    AutoTuneStatus getAutotuneStatus();

    struct PidTunings {
        double kp;
        double ki;
        double kd;
        double kp_default;
        double ki_default;
        double kd_default;
        float temp_offset_c;
    };
    PidTunings getPidTunings();
    bool resetPidToDefaults();
    
    // Getters
    KilnState getState();
    double getOutput(); // 0-100% or window time
    ThermoStats getThermoStats();
    SafetyStats getSafetyStats();
    
    // Safety
    bool isSensorHealthy();

private:
    SemaphoreHandle_t mutex;

    // Hardware
    MAX6675 *thermocouple;
    PID *pid;

    // PID Variables
    double setpoint, input, output;
    uint64_t windowStartTime;
    double runtimeKp, runtimeKi, runtimeKd;
    float temperatureOffsetC;

    // Telemetry
    uint16_t thermoLastRaw;
    uint32_t thermoReadCount;
    uint64_t lastThermoReadMs;
    bool lastSensorOk;

    // Loaded schedule metadata (for UI/history)
    std::string loadedScheduleName;

    // History (simple persistent log)
    struct HistoryPoint {
        uint64_t ts_ms;
        float temp;
        float target;
    };
    bool historyActive;
    std::string historyId;
    uint64_t historyStartMs;
    float historyPeakTemp;
    uint64_t lastHistorySampleMs;
    std::vector<HistoryPoint> historyPoints;

    // Safety / diagnostics
    float lastTemp;
    bool lastTempValid;
    uint64_t sensorHealthySinceMs;
    bool lastSensorHealthy;

    // State
    KilnState state;
    
    // Schedule
    struct ScheduleStep {
        int type; // 0=RAMP, 1=HOLD
        float value; // Temp or Time(min)
    };
    std::vector<ScheduleStep> activeSchedule;
    uint64_t stepStartTime;
    float stepStartTemp;

    // Methods
    void updateTemperature();
    void computePID();
    void driveSSR();
    void processSchedule();

    void loadRuntimeSettingsLocked();
    void persistRuntimeSettingsLocked();

    struct AutoTune {
        bool active = false;
        bool heaterOn = false;
        float setpointC = 0.0f;
        float hysteresisC = 5.0f;
        uint64_t startMs = 0;
        uint64_t lastSwitchMs = 0;
        uint64_t lastOnMs = 0;
        float peakMax = 0.0f;
        float peakMin = 0.0f;
        float lastHigh = 0.0f;
        float lastLow = 0.0f;
        int periods = 0;
        uint32_t periodMs[6] = {0};
        float ampC[6] = {0.0f};
        float ku = 0.0f;
        float pu_s = 0.0f;
        char lastError[96] = {0};
    } tune;

    void autotuneLoopLocked(uint64_t now_ms);
    void autotuneFinalizeLocked(uint64_t now_ms);
    
    void historyStartLocked(uint64_t now_ms);
    void historySampleLocked(uint64_t now_ms);
    void historyFinalizeLocked(uint64_t now_ms, const char *status);
    
    // Safety
    uint64_t lastUpdate;
};

extern ThermalController thermalCtrl;

#endif
