#ifndef GLASS_SCHEDULES_H
#define GLASS_SCHEDULES_H

#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>

// Step Types
enum StepType {
    STEP_RAMP = 0,
    STEP_HOLD = 1
};

struct KilnStep {
    StepType type;  // 0: Ramp, 1: Hold
    float rate;     // deg/hr (only for Ramp)
    float target;   // temp C
    int holdTime;   // minutes (only for Hold)

    // Convert to JSON
    JsonObject toJson(JsonDocument& doc) const {
        JsonObject obj = doc.createNestedObject();
        obj["type"] = (int)type;
        obj["rate"] = rate;
        obj["target"] = target;
        obj["hold"] = holdTime;
        return obj;
    }
};

struct KilnSchedule {
    String name;
    String description;
    std::vector<KilnStep> steps;

    // Helper to calculate estimated duration (very rough)
    int estimatedDurationMinutes() const {
        int total = 0;
        float currentTemp = 25.0; // Assume room temp start
        
        for (const auto& step : steps) {
            if (step.type == STEP_HOLD) {
                total += step.holdTime;
            } else {
                // Ramp
                float diff = abs(step.target - currentTemp);
                float degPerMin = step.rate / 60.0;
                if (degPerMin > 0) {
                    total += (diff / degPerMin);
                }
                currentTemp = step.target;
            }
        }
        return total;
    }
};

// --- Built-in Schedules ---

// Full Fuse (generic 6mm)
// Ramp 222/hr to 677, Hold 30 -> Ramp 333/hr to 804, Hold 10 -> Crash Cool
static const std::vector<KilnStep> FULL_FUSE_STEPS = {
    {STEP_RAMP, 222, 677, 0},
    {STEP_HOLD, 0, 677, 30},
    {STEP_RAMP, 333, 804, 0},
    {STEP_HOLD, 0, 804, 10},
    {STEP_RAMP, 9999, 482, 0}, // 9999 = AFAP (As Fast As Possible)
    {STEP_HOLD, 0, 482, 60},
    {STEP_RAMP, 83, 371, 0},
    {STEP_HOLD, 0, 371, 0},
    {STEP_RAMP, 9999, 25, 0}
};

// Tack Fuse
static const std::vector<KilnStep> TACK_FUSE_STEPS = {
    {STEP_RAMP, 222, 677, 0},
    {STEP_HOLD, 0, 677, 30},
    {STEP_RAMP, 333, 730, 0}, // Lower peak
    {STEP_HOLD, 0, 730, 10},
    {STEP_RAMP, 9999, 482, 0},
    {STEP_HOLD, 0, 482, 60},
    {STEP_RAMP, 83, 371, 0},
    {STEP_HOLD, 0, 371, 0},
    {STEP_RAMP, 9999, 25, 0}
};

#endif
