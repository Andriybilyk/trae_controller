#ifndef THERMAL_CONTROL_H
#define THERMAL_CONTROL_H

#include <vector>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "PID.h"
#include "MAX6675.h"
#include "config.h"

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
    int currentStep;
    int totalSteps;
};

class ThermalController {
public:
    ThermalController();
    void begin();
    void loop(); // Call this in a loop or task

    // Commands
    void startSchedule(const std::string& scheduleJson);
    void loadSchedule(const std::string& scheduleJson);
    void start();
    void stop(const std::string& reason = std::string("User Request"));
    void emergencyStop(const std::string& reason);
    void skipCurrentStep();
    void addTemperatureToTarget(float tempToAdd);
    void addTimeToHold(float minutesToAdd);
    
    // Getters
    KilnState getState();
    double getOutput(); // 0-100% or window time
    
    // Safety
    bool isSensorHealthy();

private:
    SemaphoreHandle_t mutex;

    // Hardware
    MAX6675* thermocouple;
    PID* pid;

    // PID Variables
    double setpoint, input, output;
    uint64_t windowStartTime;

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
    
    // Safety
    uint64_t lastUpdate;
};

extern ThermalController thermalCtrl;

#endif
