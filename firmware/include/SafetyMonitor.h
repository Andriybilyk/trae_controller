#ifndef SAFETY_MONITOR_H
#define SAFETY_MONITOR_H

#include <Arduino.h>

class SafetyMonitor {
public:
    enum SafetyStatus {
        SAFE,
        ERROR_SENSOR_FAIL,
        ERROR_HEATER_STUCK_ON,
        ERROR_HEATER_FAIL,
        ERROR_OVERTEMP,
        ERROR_COLD_JUNCTION_OVERHEAT, // Electronics Overheat
        ERROR_THERMOCOUPLE_OPEN       // Broken Wire
    };

    // Configuration
    const double MAX_SAFE_TEMP = 1300.0;
    const double MIN_SAFE_TEMP = 0.0;     // Below freezing usually means error for kiln
    const double MAX_COLD_JUNCTION = 60.0; // PCB Temp Limit
    
    // Stall Detection
    unsigned long stallStartTime = 0;
    const unsigned long STALL_TIMEOUT = 600000; // 10 minutes
    const double STALL_TEMP_RISE = 5.0;         // Must rise 5C
    double stallStartTemp = 0;

    SafetyStatus check(double currentTemp, double coldJunctionTemp, double outputPercent) {
        // 1. Thermocouple Open / NAN
        if (isnan(currentTemp)) return ERROR_SENSOR_FAIL;
        
        // 2. Out of Range (Broken Wire or Short)
        if (currentTemp > MAX_SAFE_TEMP || currentTemp < MIN_SAFE_TEMP) return ERROR_THERMOCOUPLE_OPEN;

        // 3. Cold Junction (Electronics) Overheat
        if (coldJunctionTemp > MAX_COLD_JUNCTION) return ERROR_COLD_JUNCTION_OVERHEAT;
        
        // 4. Stall Protection (Heater Fail)
        if (outputPercent > 80.0) {
            if (stallStartTime == 0) {
                stallStartTime = millis();
                stallStartTemp = currentTemp;
            } else if (millis() - stallStartTime > STALL_TIMEOUT) {
                if (currentTemp - stallStartTemp < STALL_TEMP_RISE) {
                    return ERROR_HEATER_FAIL; // Stuck!
                } else {
                    // Reset if we made progress
                    stallStartTime = millis();
                    stallStartTemp = currentTemp;
                }
            }
        } else {
            stallStartTime = 0;
        }

        return SAFE;
    }
};

#endif