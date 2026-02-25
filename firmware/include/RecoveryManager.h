#ifndef RECOVERY_MANAGER_H
#define RECOVERY_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>
#include "glass_schedules.h" // For Step struct

struct RecoveryState {
    bool wasFiring;
    int currentStepIndex;
    unsigned long timeInStep; // ms elapsed in current step
    float lastTargetTemp;
    String scheduleName;
    std::vector<KilnStep> schedule; // We need to save the schedule itself too
    float lastMeasuredTemp;
};

class RecoveryManager {
public:
    const char* FILE_PATH = "/recovery.json";

    void saveState(bool isFiring, int stepIndex, unsigned long timeInStep, float targetTemp, float currentTemp, String name, const std::vector<KilnStep>& schedule) {
        // Optimization: Don't save if not firing and file doesn't exist (to save flash writes)
        if (!isFiring && !LittleFS.exists(FILE_PATH)) return;

        DynamicJsonDocument doc(4096);
        doc["firing"] = isFiring;
        
        if (isFiring) {
            doc["step"] = stepIndex;
            doc["time"] = timeInStep;
            doc["target"] = targetTemp;
            doc["lastTemp"] = currentTemp;
            doc["name"] = name;
            
            JsonArray steps = doc.createNestedArray("schedule");
            for (const auto& s : schedule) {
                // Use the built-in toJson method
                s.toJson(doc);
                // Note: toJson adds to doc directly, but we need to add it to the array.
                // The toJson implementation in glass_schedules.h returns a JsonObject.
                // Let's fix this logic.
                // s.toJson(doc) creates a nested object on doc, but we want it inside 'steps'.
                // Actually, let's just do it manually here or update the helper.
                // Reverting to manual for safety or using the helper correctly.
                
                JsonObject stepObj = steps.createNestedObject();
                stepObj["type"] = (int)s.type;
                stepObj["rate"] = s.rate;
                stepObj["target"] = s.target;
                stepObj["hold"] = s.holdTime;
            }
        }

        File file = LittleFS.open(FILE_PATH, "w");
        if (file) {
            serializeJson(doc, file);
            file.close();
        }
    }

    bool loadState(RecoveryState& state) {
        if (!LittleFS.exists(FILE_PATH)) return false;

        File file = LittleFS.open(FILE_PATH, "r");
        if (!file) return false;

        DynamicJsonDocument doc(4096);
        DeserializationError error = deserializeJson(doc, file);
        file.close();

        if (error) return false;

        state.wasFiring = doc["firing"] | false;
        if (!state.wasFiring) return false;

        state.currentStepIndex = doc["step"] | 0;
        state.timeInStep = doc["time"] | 0;
        state.lastTargetTemp = doc["target"] | 0.0;
        state.lastMeasuredTemp = doc["lastTemp"] | 0.0;
        state.scheduleName = doc["name"].as<String>();

        JsonArray steps = doc["schedule"];
        for (JsonVariant v : steps) {
            KilnStep s;
            s.type = (StepType)(v["type"] | 0);
            s.rate = v["rate"];
            s.target = v["target"];
            s.holdTime = v["hold"];
            state.schedule.push_back(s);
        }

        return true;
    }

    void clearState() {
        if (LittleFS.exists(FILE_PATH)) {
            LittleFS.remove(FILE_PATH);
        }
    }
};

#endif