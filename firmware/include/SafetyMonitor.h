#ifndef SAFETY_MONITOR_H
#define SAFETY_MONITOR_H

#include <Arduino.h>

class SafetyMonitor {
public:
    enum SafetyStatus {
        SAFE,
        ERROR_SENSOR_FAIL,
        ERROR_HEATER_STUCK_ON,  // Relay failed closed (dangerous!)
        ERROR_HEATER_FAIL,      // Element burned out or relay failed open
        ERROR_SLOW_RESPONSE,    // Lag alarm
        ERROR_OVERTEMP          // Absolute max limit
    };

    SafetyMonitor(float maxTempLimit = 1300.0) 
        : _maxTempLimit(maxTempLimit) {}

    void reset() {
        _status = SAFE;
        _lastCheckTime = millis();
        _heatingStartTime = 0;
        _coolingStartTime = 0;
        _startTemp = 0;
    }

    SafetyStatus update(float currentTemp, float targetTemp, float outputPercent) {
        unsigned long now = millis();
        
        // 0. Check Absolute Limit
        if (currentTemp > _maxTempLimit) return ERROR_OVERTEMP;
        if (isnan(currentTemp)) return ERROR_SENSOR_FAIL;

        // 1. Check Heater Stuck ON (Runaway)
        // If output is 0% but temp rises significantly (> 10C in 2 mins)
        if (outputPercent < 1.0) { // Off
            if (_coolingStartTime == 0) {
                _coolingStartTime = now;
                _startTemp = currentTemp;
            } else if (now - _coolingStartTime > 120000) { // 2 mins
                if (currentTemp > _startTemp + 10.0) {
                    return ERROR_HEATER_STUCK_ON;
                }
                // Reset window
                _coolingStartTime = now;
                _startTemp = currentTemp;
            }
            _heatingStartTime = 0; // Reset heating logic
        } 
        
        // 2. Check Heater Fail (Lag)
        // If output is > 90% but temp doesn't rise (> 2C in 5 mins)
        else if (outputPercent > 90.0) {
            if (_heatingStartTime == 0) {
                _heatingStartTime = now;
                _startTemp = currentTemp;
            } else if (now - _heatingStartTime > 300000) { // 5 mins
                if (currentTemp < _startTemp + 2.0) {
                    return ERROR_HEATER_FAIL;
                }
                // Reset window
                _heatingStartTime = now;
                _startTemp = currentTemp;
            }
            _coolingStartTime = 0; // Reset cooling logic
        } else {
            // Normal operation (partial power)
            _heatingStartTime = 0;
            _coolingStartTime = 0;
        }

        return SAFE;
    }

private:
    float _maxTempLimit;
    SafetyStatus _status = SAFE;
    unsigned long _lastCheckTime = 0;
    
    // For tracking trends
    unsigned long _heatingStartTime = 0;
    unsigned long _coolingStartTime = 0;
    float _startTemp = 0;
};

#endif