#ifndef THERMAL_CONTROL_H
#define THERMAL_CONTROL_H

#include <cstdint>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "kiln_safety/faults.h"

class MAX31855;
class PID;

enum KilnStatus {
    KILN_IDLE = 0,
    KILN_RUNNING = 1,
    KILN_HOLD = 2,
    KILN_COOLING = 3,
    KILN_COMPLETE = 4,
    KILN_FAULT = 5,
    KILN_PAUSED = 6,
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
    uint32_t raw;
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
    bool loadSchedule(const std::string& scheduleJson);
    void start();
    void stop(const std::string& reason = std::string("User Request"));
    bool startAutotune(float setpointC);
    void stopAutotune(const std::string& reason = std::string("User Request"));
    void emergencyStop(const std::string& reason);
    bool clearFault();
    bool skipCurrentStep();
    bool addTemperatureToTarget(float tempToAdd);
    bool addTimeToHold(float minutesToAdd);
    bool setCurrentRampRate(float rateCPerMin);
    bool setTemperatureOffset(float offsetC);
    float getTemperatureOffset();
    bool setUserMaxTemperatureC(float maxC);
    float getUserMaxTemperatureC();
    float getLoadedScheduleMaxTargetC();
    bool setAutotuneTargetC(float targetC);
    float getAutotuneTargetC();

    struct AutoTuneStatus {
        bool active;
        bool heaterOn;
        float setpointC;
        int cycles;
        int valid_cycles;
        int total_cycles;
        float ku;
        float pu_s;
        float period_cv;
        float amp_cv;
        float quality;
        float confidence;
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
    MAX31855 *thermocouple;
    PID *pid;

    // PID Variables
    double setpoint, input, output;
    uint64_t windowStartTime;
    double runtimeKp, runtimeKi, runtimeKd;
    float temperatureOffsetC;
    float userMaxTempC;
    float autotuneTargetC;

    // Telemetry
    uint32_t thermoLastRaw;
    uint32_t thermoReadCount;
    uint64_t lastThermoReadMs;
    bool lastSensorOk;
    uint32_t sensorValidStreak;
    uint32_t sensorInvalidStreak;
    uint32_t sensorStuckStreak;
    uint32_t sensorShortStreak;

    // Loaded schedule metadata (for UI/history)
    std::string loadedScheduleName;

    // History (simple persistent log)
    struct HistoryPoint {
        uint64_t ts_ms;
        float temp;
        float target;
    };
    struct HistoryChange {
        uint64_t ts_ms;
        int step;
        std::string action;
        std::string field;
        float before_value;
        float after_value;
        float delta;
    };
    bool historyActive;
    std::string historyId;
    uint64_t historyStartMs;
    uint64_t historyStartUnixMs;
    float historyPeakTemp;
    uint64_t lastHistorySampleMs;
    std::vector<HistoryPoint> historyPoints;
    std::vector<HistoryChange> historyChanges;

    // Safety / diagnostics
    float lastTemp;
    bool lastTempValid;
    uint64_t sensorHealthySinceMs;
    bool lastSensorHealthy;

    float tempFilterBuf[5];
    uint8_t tempFilterCount;
    uint8_t tempFilterIndex;

    // State
    KilnState state;
    
    // Schedule
    struct ScheduleStep {
        int type; // 0=RAMP, 1=HOLD
        float value; // Temp or Time(min)
        float rate; // C/min for RAMP
    };
    std::vector<ScheduleStep> activeSchedule;
    uint64_t stepStartTime;
    float stepStartTemp;

    // Methods
    void tripFaultLocked(kiln_fault_code_t code, const char *reason);
    void applyFaultStateLocked();
    bool sensorRecoveredForClearLocked(uint64_t now_ms) const;
    void updateTemperature();
    void computePID();
    void driveSSR();
    void processSchedule();

    void loadRuntimeSettingsLocked();
    void persistRuntimeSettingsLocked();
    void saveRecoveryStateLocked();
    void loadRecoveryStateLocked();
    void clearRecoveryStateLocked();

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
        float periodCv = 0.0f;
        float ampCv = 0.0f;
        float quality = 0.0f;
        float confidence = 0.0f;
        int validCycles = 0;
        int totalCycles = 0;
        char lastError[96] = {0};
    } tune;

    void autotuneLoopLocked(uint64_t now_ms);
    void autotuneFinalizeLocked(uint64_t now_ms);
    
    void historyStartLocked(uint64_t now_ms);
    void historySampleLocked(uint64_t now_ms);
    void historyFinalizeLocked(uint64_t now_ms, const char *status);
    void historyRecordChangeLocked(const char *action, const char *field, float before_value, float after_value);
    
    // Safety
    uint64_t lastUpdate;
};

extern ThermalController thermalCtrl;

#endif
