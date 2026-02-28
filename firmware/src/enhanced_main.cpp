#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_MAX6675.h>
#include <PID_v1.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ESPmDNS.h>

#include "config.h"
#include "glass_schedules.h"
#include "SafetyMonitor.h" // Professional Safety Class
#include "RecoveryManager.h" // Power Loss Recovery
#include <esp_task_wdt.h>
#include <Preferences.h>
#include <time.h> // NTP Support

#include <WiFiManager.h> // WiFi Manager

// Watchdog Timeout (Seconds)
#define WDT_TIMEOUT 10

// NTP Server Settings
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7200; // GMT+2 (Ukraine Standard)
const int   daylightOffset_sec = 3600; // Summer Time (+1h)

// Preferences (NVS Storage)
Preferences preferences;

// WiFi Fail-Safe
unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 30000; // 30 sec
bool wifiConnected = false;

// --- Global Objects ---
Adafruit_MAX6675 thermocouple(MAXCLK, MAXCS, MAXDO);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
SafetyMonitor safetyMonitor(1300.0); // 1300C limit
RecoveryManager recoveryManager;

// --- PID Variables ---
// Using 3 independent PIDs for multi-zone support
double Setpoint[3], Input[3], Output[3];

// Structure to hold PID tunings for different temperature ranges
struct PIDTuning {
    float temp; // The upper temperature for this tuning
    double kp, ki, kd;
};

std::vector<PIDTuning> pidTunings;

// Default tuning if none are loaded
double Kp = PID_KP, Ki = PID_KI, Kd = PID_KD;

PID pid0(&Input[0], &Output[0], &Setpoint[0], Kp, Ki, Kd, DIRECT);
PID pid1(&Input[1], &Output[1], &Setpoint[1], Kp, Ki, Kd, DIRECT);
PID pid2(&Input[2], &Output[2], &Setpoint[2], Kp, Ki, Kd, DIRECT);

// --- State Machine ---
enum KilnStatus { IDLE, PREHEAT, RAMP, HOLD, COOL, COMPLETE, ERROR, PAUSED, TUNING };
const char* statusStr[] = {"IDLE", "PREHEAT", "RAMP", "HOLD", "COOL", "COMPLETE", "ERROR", "PAUSED", "TUNING"};

struct KilnState {
    KilnStatus status = IDLE;
    bool isFiring = false;
    float currentTemp[3] = {0.0, 0.0, 0.0};
    float targetTemp = 0.0;
    unsigned long startTime = 0;      // When the firing started
    unsigned long stepStartTime = 0;  // When the current step started
    
    // Guaranteed Soak
    unsigned long totalHoldPauseTime = 0; // Total time paused during hold
    unsigned long holdPauseStart = 0;     // When current pause started
    bool isHoldPaused = false;            // Is hold currently paused?
    
    // AutoTune
    bool isTuning = false;
    int tuneStep = 0;
    unsigned long tuneStartTime = 0;
    float tuneTarget = 150.0; // Tuning temperature
    float tunePeak = 0;
    float tuneValley = 9999;
    unsigned long tunePeriod = 0;
    
    int currentStepIndex = 0;
    String errorMsg = "";
    unsigned long lastUpdate = 0;
    float energyUsed = 0.0; // kWh estimate
};

KilnState state;

// --- Schedule Model ---
// Step struct is now defined in glass_schedules.h

std::vector<KilnStep> activeSchedule;
String activeScheduleName = "";

// --- Safety ---
unsigned long lastSensorRead = 0;
bool sensorError = false;

// --- Function Prototypes ---
void setupWiFi();
void setupAPI();
void setupMQTT();
void setupBLE();
void runControlLoop();
void updateSafety();
void loadConfig();
void saveConfig();
void broadcastStatus();
void startFiring(const std::vector<KilnStep>& schedule, String name);
void stopFiring();
void handleMQTT();

void saveTunings() {
    preferences.begin("pid-tunings", false);
    preferences.clear(); // Clear old tunings
    preferences.putUInt("count", pidTunings.size());
    for (int i = 0; i < pidTunings.size(); i++) {
        String prefix = "t" + String(i) + "_";
        preferences.putFloat((prefix + "temp").c_str(), pidTunings[i].temp);
        preferences.putDouble((prefix + "kp").c_str(), pidTunings[i].kp);
        preferences.putDouble((prefix + "ki").c_str(), pidTunings[i].ki);
        preferences.putDouble((prefix + "kd").c_str(), pidTunings[i].kd);
    }
    preferences.end();
    Serial.println("Saved PID tunings to NVS.");
}

void loadTunings() {
    preferences.begin("pid-tunings", true); // Read-only
    unsigned int count = preferences.getUInt("count", 0);
    pidTunings.clear();
    if (count > 0) {
        Serial.printf("Loading %u PID tuning sets...\n", count);
        for (int i = 0; i < count; i++) {
            String prefix = "t" + String(i) + "_";
            PIDTuning tuning;
            tuning.temp = preferences.getFloat((prefix + "temp").c_str(), 0);
            tuning.kp = preferences.getDouble((prefix + "kp").c_str(), PID_KP);
            tuning.ki = preferences.getDouble((prefix + "ki").c_str(), PID_KI);
            tuning.kd = preferences.getDouble((prefix + "kd").c_str(), PID_KD);
            pidTunings.push_back(tuning);
        }
        // Sort by temperature
        std::sort(pidTunings.begin(), pidTunings.end(), [](const PIDTuning& a, const PIDTuning& b) {
            return a.temp < b.temp;
        });
    } else {
        Serial.println("No PID tunings found, using defaults.");
        // Add a default tuning if none exist
        pidTunings.push_back({1300, Kp, Ki, Kd});
    }
    preferences.end();
}

// --- Setup ---
void setup() {
    Serial.begin(115200);
    
    // Init File System
    if(!LittleFS.begin(true)){
        Serial.println("LittleFS Mount Failed");
        return;
    }
    loadConfig();

    // Load PID Tunings from NVS
    loadTunings();

    // Init Hardware
    pinMode(SSR_ZONE1_PIN, OUTPUT);
    digitalWrite(SSR_ZONE1_PIN, LOW);

    // Init Safety Hardware
    pinMode(SAFETY_RELAY_PIN, OUTPUT);
    digitalWrite(SAFETY_RELAY_PIN, LOW); // Relay OFF (Safety Cutoff)
    
    pinMode(FAN_PIN, OUTPUT);
    digitalWrite(FAN_PIN, LOW); // Fan OFF
    
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW); // Buzzer OFF
    
    if (!thermocouple.begin()) {
        state.errorMsg = "Thermocouple Init Failed!";
        state.status = ERROR;
        Serial.println("ERROR: MAX6675 not found.");
        sensorError = true;
    }

    // Load Preferences
    preferences.begin("kiln-config", false);
    double savedKp = preferences.getDouble("kp", 2.0);
    double savedKi = preferences.getDouble("ki", 5.0);
    double savedKd = preferences.getDouble("kd", 1.0);
    int savedOffset = preferences.getInt("offset", 0);
    preferences.end();
    
    // Apply saved config
    Kp = savedKp; Ki = savedKi; Kd = savedKd;
    pid0.SetTunings(Kp, Ki, Kd);
    // TODO: Apply thermocouple offset if supported by library wrapper

    // Init PID
    pid0.SetMode(AUTOMATIC);
    pid0.SetOutputLimits(0, PID_WINDOW_SIZE);
    
    // For zones 2 and 3, init if enabled
    pid1.SetMode(AUTOMATIC);
    pid1.SetOutputLimits(0, PID_WINDOW_SIZE);
    pid2.SetMode(AUTOMATIC);
    pid2.SetOutputLimits(0, PID_WINDOW_SIZE);

    // Init Watchdog (Hardware WDT)
    esp_task_wdt_init(WDT_TIMEOUT, true); // Panic on timeout (reset)
    esp_task_wdt_add(NULL); // Watch current task (loop)

    // WiFi Setup (WiFiManager)
    WiFiManager wm;
    // Set timeout to avoid blocking startup forever (e.g., 3 mins)
    wm.setConfigPortalTimeout(180); 
    
    bool res = wm.autoConnect("Trae Kiln Setup"); // AP Name
    
    if(!res) {
        Serial.println("Failed to connect or hit timeout");
        // ESP.restart(); // Optional: restart or continue offline
    } else {
        Serial.println("WiFi Connected!");
        Serial.println(WiFi.localIP());
        // --- Start mDNS Service ---
        if (MDNS.begin("kiln")) {
            Serial.println("mDNS responder started. Hostname: kiln.local");
            MDNS.addService("http", "tcp", 80);
            MDNS.addService("ws", "tcp", 80);
        } else {
            Serial.println("Error starting mDNS");
        }
    }

    // Init NTP
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    // We don't wait for WiFi here to ensure critical safety loops start immediately
    
    // Web Server Routes
    setupAPI();

    // --- Power Recovery Check ---
    RecoveryState recovered;
    if (recoveryManager.loadState(recovered) && recovered.wasFiring) {
        Serial.println("Found interrupted firing session!");
        // Check temp drop
        float currentT = thermocouple.readCelsius();
        if (!isnan(currentT)) {
            float drop = recovered.lastMeasuredTemp - currentT;
            if (drop < 50.0 && drop > -50.0) { // Allow some fluctuation
                Serial.println("Recovering firing...");
                activeSchedule = recovered.schedule;
                activeScheduleName = recovered.scheduleName;
                state.isFiring = true;
                state.status = (recovered.schedule[recovered.currentStepIndex].type == STEP_RAMP) ? RAMP : HOLD;
                state.currentStepIndex = recovered.currentStepIndex;
                
                // Restore time
                // We need to set stepStartTime such that millis() - stepStartTime = recovered.timeInStep
                state.stepStartTime = millis() - recovered.timeInStep;
                state.startTime = millis(); // Approximate
                
                state.errorMsg = "Recovered from Power Loss";
            } else {
                state.errorMsg = "Power Loss: Temp drop too large to resume";
                state.status = ERROR;
                recoveryManager.clearState();
            }
        }
    }

    Serial.println("Kiln Controller Ready");
}

// --- Main Loop ---
void loop() {
    // 0. WATCHDOG RESET
    esp_task_wdt_reset(); // Feed the dog!

    unsigned long now = millis();

    // WiFi Connectivity Check (Non-blocking)
    if (now - lastWiFiCheck >= WIFI_CHECK_INTERVAL) {
        lastWiFiCheck = now;
        if (WiFi.status() == WL_CONNECTED) {
            if (!wifiConnected) {
                Serial.println("WiFi Connected!");
                Serial.println(WiFi.localIP());
                wifiConnected = true;
            }
        } else {
            Serial.println("WiFi Lost... Reconnecting (Async)");
            WiFi.disconnect();
            WiFi.reconnect();
            wifiConnected = false;
        }
    }

    // 1. Read Sensors (every 500ms)
    if (now - lastSensorRead > 500) {
        double c = thermocouple.readCelsius();
        if (isnan(c)) {
            state.errorMsg = "Sensor Read Error";
            // If firing, this is critical
            if (state.isFiring) {
                stopFiring();
                state.status = ERROR;
            }
        } else {
            state.currentTemp[0] = c;
            Input[0] = c;
        }
        lastSensorRead = now;
    }

    // 2. Control Logic
    if (state.isFiring && state.status != ERROR && state.status != PAUSED) {
        runControlLoop();
    } else {
        digitalWrite(SSR_ZONE1_PIN, LOW);
        state.targetTemp = 0;
        Output[0] = 0;
    }

    // 3. Telemetry (every 1s)
    if (now - state.lastUpdate > 1000) {
        // Calculate Energy Used (Simple estimation)
        // Assume 3kW heater. Output is 0-2000ms window.
        // Duty cycle = Output / 2000.
        // Energy (kWh) += (Duty * 3kW * (1/3600)h)
        float duty = Output[0] / PID_WINDOW_SIZE;
        float powerKw = 3.0; // Default, should come from config
        state.energyUsed += (duty * powerKw * (1.0/3600.0));
        
        broadcastStatus();
        state.lastUpdate = now;
        
        // --- Recovery Save (Every minute) ---
        static unsigned long lastSave = 0;
        if (now - lastSave > 60000 && state.isFiring) {
            unsigned long timeInStep = now - state.stepStartTime;
            recoveryManager.saveState(true, state.currentStepIndex, timeInStep, state.targetTemp, state.currentTemp[0], activeScheduleName, activeSchedule);
            lastSave = now;
        }
    }

    ws.cleanupClients();
    handleMQTT();
    
    // 1. SAFETY FIRST
    // Check sensors and safety logic every loop (critical)
    float currentOutput = (Output[0] / PID_WINDOW_SIZE) * 100.0; // 0-100%
    double internalTemp = 25.0; // Dummy value, MAX6675 has no internal sensor
    SafetyMonitor::SafetyStatus safetyStatus = safetyMonitor.check(state.currentTemp[0], internalTemp, currentOutput);
    
    if (safetyStatus != SafetyMonitor::SAFE) {
        stopFiring();
        state.status = ERROR;
        
        switch (safetyStatus) {
            case SafetyMonitor::ERROR_SENSOR_FAIL:
                state.errorMsg = "Critical: Sensor Failure (NAN)!";
                break;
            case SafetyMonitor::ERROR_THERMOCOUPLE_OPEN:
                state.errorMsg = "Critical: Thermocouple Open/Broken!";
                break;
                        /* case SafetyMonitor::ERROR_COLD_JUNCTION_OVERHEAT:
                state.errorMsg = "Critical: Electronics Overheat! Fan ON";
                digitalWrite(FAN_PIN, HIGH); // Emergency Cooling
                break; // NOTE: MAX6675 does not have a cold junction sensor */
            case SafetyMonitor::ERROR_HEATER_STUCK_ON:
                state.errorMsg = "Critical: Relay Stuck ON!";
                // TODO: Trigger external alarm or mechanical relay
                break;
            case SafetyMonitor::ERROR_HEATER_FAIL:
                state.errorMsg = "Error: Heater Stalled (No Temp Rise)";
                break;
            case SafetyMonitor::ERROR_OVERTEMP:
                state.errorMsg = "Critical: Over Temperature!";
                break;
            default:
                state.errorMsg = "Unknown Safety Error";
        }
        Serial.println(state.errorMsg);
        
        // Alarm
        for(int i=0; i<3; i++) {
            digitalWrite(BUZZER_PIN, HIGH);
            delay(200);
            digitalWrite(BUZZER_PIN, LOW);
            delay(200);
        }
    }
}

void applyGainScheduling() {
    if (pidTunings.empty()) return;

    // Find the best tuning for the current setpoint
    PIDTuning bestTuning = pidTunings[0]; // Default to the first one
    for (const auto& tuning : pidTunings) {
        if (Setpoint[0] <= tuning.temp) {
            bestTuning = tuning;
            break;
        }
        bestTuning = tuning; // Use the last one if setpoint is higher than all table values
    }

    // Check if the tuning has changed
    if (abs(pid0.GetKp() - bestTuning.kp) > 0.01 || abs(pid0.GetKi() - bestTuning.ki) > 0.01 || abs(pid0.GetKd() - bestTuning.kd) > 0.01) {
        pid0.SetTunings(bestTuning.kp, bestTuning.ki, bestTuning.kd);
        pid1.SetTunings(bestTuning.kp, bestTuning.ki, bestTuning.kd);
        pid2.SetTunings(bestTuning.kp, bestTuning.ki, bestTuning.kd);
        Serial.printf("PID Gain Scheduling: Applied tunings for %.0fC (Kp:%.2f Ki:%.2f Kd:%.2f)\n", bestTuning.temp, bestTuning.kp, bestTuning.ki, bestTuning.kd);
    }
}

// --- Control Logic ---
void runControlLoop() {
    // --- AUTOTUNE MODE ---
    if (state.isTuning) {
        state.status = TUNING;
        float current = state.currentTemp[0];
        
        // Step 0: Heat to target
        if (state.tuneStep == 0) {
            state.targetTemp = state.tuneTarget;
            if (current < state.tuneTarget) Output[0] = PID_WINDOW_SIZE; // Full ON
            else {
                state.tuneStep = 1; // Reached target
                state.tuneStartTime = millis();
                state.tunePeak = current;
                Output[0] = 0; // Turn OFF
            }
        }
        // Step 1: Wait for peak (overshoot) then drop below target
        else if (state.tuneStep == 1) {
            Output[0] = 0; // Stay OFF
            if (current > state.tunePeak) state.tunePeak = current;
            if (current < state.tuneTarget) {
                 state.tuneStep = 2; // Crossed below
                 state.tuneValley = current;
            }
        }
        // Step 2: Wait for valley (undershoot) then rise above target
        else if (state.tuneStep == 2) {
            Output[0] = PID_WINDOW_SIZE; // Full ON
            if (current < state.tuneValley) state.tuneValley = current;
            if (current > state.tuneTarget) {
                // Cycle Complete! Calculate PID
                float amplitude = (state.tunePeak - state.tuneValley) / 2.0;
                float period = (millis() - state.tuneStartTime) / 1000.0; // Seconds
                
                // Ziegler-Nichols (Relay)
                Kp = 50.0 / amplitude; 
                Ki = Kp / (period / 2.0);
                Kd = Kp * (period / 8.0);
                
                // --- Update Tuning Table ---
                // Remove any existing tuning for a similar temperature range
                pidTunings.erase(
                    std::remove_if(pidTunings.begin(), pidTunings.end(),
                        [&](const PIDTuning& t) {
                            return abs(t.temp - state.tuneTarget) < 50; // Remove if within 50C
                        }),
                    pidTunings.end()
                );

                // Add the new tuning
                pidTunings.push_back({state.tuneTarget, Kp, Ki, Kd});

                // Sort the vector by temperature
                std::sort(pidTunings.begin(), pidTunings.end(), [](const PIDTuning& a, const PIDTuning& b) {
                    return a.temp < b.temp;
                });

                // Save to NVS
                saveTunings();
                
                Serial.printf("AutoTune Complete: Kp=%.2f Ki=%.2f Kd=%.2f for %.0fC\n", Kp, Ki, Kd, state.tuneTarget);
                state.isTuning = false;
                state.status = COMPLETE;
                stopFiring();
            }
        }
        return; // Skip normal PID
    }

    if (activeSchedule.empty()) return;

    KilnStep currentStep = activeSchedule[state.currentStepIndex];
    unsigned long timeInStep = millis() - state.stepStartTime; // ms
    float minutesInStep = timeInStep / 60000.0;

    // Determine Setpoint based on Ramp or Hold
    if (currentStep.type == STEP_RAMP) { // Ramp
        // Calculate target based on rate
        // rate is deg/hr -> deg/min = rate / 60
        // We need previous step temp (or start temp)
        float startTemp = (state.currentStepIndex == 0) ? 25.0 : activeSchedule[state.currentStepIndex - 1].target;
        
        float degPerMin = currentStep.rate / 60.0;
        if (currentStep.rate == 0 || currentStep.rate >= 9999) degPerMin = 1000; // "As fast as possible"
        
        float calculatedSetpoint = startTemp + (degPerMin * minutesInStep);
        
        // Cap at target
        if ((degPerMin > 0 && calculatedSetpoint >= currentStep.target) || 
            (degPerMin < 0 && calculatedSetpoint <= currentStep.target)) {
            // Step Complete
            state.currentStepIndex++;
            state.stepStartTime = millis();
            if (state.currentStepIndex >= activeSchedule.size()) {
                state.status = COMPLETE;
                stopFiring();
            }
        } else {
            state.status = RAMP;
            Setpoint[0] = calculatedSetpoint;
            
            // Reset hold pause vars for next step
            state.totalHoldPauseTime = 0;
            state.isHoldPaused = false;
        }
    } else if (currentStep.type == STEP_HOLD) { // Hold
        state.status = HOLD;
        Setpoint[0] = currentStep.target;
        
        // --- Guaranteed Soak Logic ---
        float deviation = Setpoint[0] - state.currentTemp[0];
        
        // If temp is too low (e.g., > 3C below target), PAUSE the timer
        // But only if we are not just starting the hold (allow some settling time? No, strict is better for glass)
        // Let's use 2.0C hysteresis
        if (deviation > 3.0) {
            if (!state.isHoldPaused) {
                state.isHoldPaused = true;
                state.holdPauseStart = millis();
                Serial.println("HOLD PAUSED: Temp too low");
            }
        } else if (deviation <= 1.0) { // Resume when close enough
            if (state.isHoldPaused) {
                state.isHoldPaused = false;
                state.totalHoldPauseTime += (millis() - state.holdPauseStart);
                Serial.println("HOLD RESUMED");
            }
        }
        
        // Effective time in step = (Total elapsed) - (Total paused) - (Current pause duration if active)
        unsigned long currentPauseDuration = state.isHoldPaused ? (millis() - state.holdPauseStart) : 0;
        unsigned long effectiveTime = timeInStep - state.totalHoldPauseTime - currentPauseDuration;
        float effectiveMinutes = effectiveTime / 60000.0;
        
        if (effectiveMinutes >= currentStep.holdTime) {
             // Step Complete
            state.currentStepIndex++;
            state.stepStartTime = millis();
            state.totalHoldPauseTime = 0; // Reset
            state.isHoldPaused = false;
            
             if (state.currentStepIndex >= activeSchedule.size()) {
                state.status = COMPLETE;
                stopFiring();
            }
        }
    }

    state.targetTemp = Setpoint[0];
    Setpoint[1] = Setpoint[0]; // All zones follow same schedule
    Setpoint[2] = Setpoint[0];

    // Apply Gain Scheduling
    applyGainScheduling();

    // PID Compute
    pid0.Compute();
    pid1.Compute();
    pid2.Compute();

    // Time Proportional Output
    static unsigned long windowStartTime = 0;
    if (millis() - windowStartTime > PID_WINDOW_SIZE) {
        windowStartTime = millis();
    }
    
    // Zone 1
    if (Output[0] > (millis() - windowStartTime)) digitalWrite(SSR_ZONE1_PIN, HIGH);
    else digitalWrite(SSR_ZONE1_PIN, LOW);

    // Zone 2 (if enabled)
    #ifdef SSR_ZONE2_PIN
    if (Output[1] > (millis() - windowStartTime)) digitalWrite(SSR_ZONE2_PIN, HIGH);
    else digitalWrite(SSR_ZONE2_PIN, LOW);
    #endif

    // Zone 3 (if enabled)
    #ifdef SSR_ZONE3_PIN
    if (Output[2] > (millis() - windowStartTime)) digitalWrite(SSR_ZONE3_PIN, HIGH);
    else digitalWrite(SSR_ZONE3_PIN, LOW);
    #endif
}

// --- Helper Functions ---
void startFiring(const std::vector<Step>& schedule, String name) {
    activeSchedule = schedule;
    activeScheduleName = name;
    state.isFiring = true;
    state.status = RAMP; // Assume start with ramp
    state.startTime = millis();
    state.stepStartTime = millis();
    state.currentStepIndex = 0;
    state.errorMsg = "";
    
    // Safety: Enable Power
    digitalWrite(SAFETY_RELAY_PIN, HIGH);
    digitalWrite(FAN_PIN, HIGH);
    
    // Beep to confirm start
    digitalWrite(BUZZER_PIN, HIGH);
    delay(200);
    digitalWrite(BUZZER_PIN, LOW);
    
    // Reset PID
    pid0.SetMode(AUTOMATIC);
}

void stopFiring() {
    state.isFiring = false;
    state.status = IDLE;
    digitalWrite(SSR_ZONE1_PIN, LOW);
    
    // Safety: Cut Power
    digitalWrite(SAFETY_RELAY_PIN, LOW);
    digitalWrite(FAN_PIN, LOW); // Fan off (or could keep on for cooling)
    
    // Beep to confirm stop
    digitalWrite(BUZZER_PIN, HIGH);
    delay(500);
    digitalWrite(BUZZER_PIN, LOW);
    
    recoveryManager.clearState(); // Clear recovery file
}

void broadcastStatus() {
    DynamicJsonDocument doc(512);
    doc["temp"] = state.currentTemp[0];
    doc["target"] = state.targetTemp;
    doc["status"] = statusStr[state.status];
    doc["step"] = state.currentStepIndex;
    doc["totalSteps"] = activeSchedule.size();
    doc["output"] = Output[0]; // 0-2000
    doc["error"] = state.errorMsg;
    doc["energy"] = state.energyUsed;
    
    // Simulate multi-zone for demo (remove in production)
    doc["t2"] = state.currentTemp[0] - 2.5; 
    doc["t3"] = state.currentTemp[0] + 1.2;

    String json;
    serializeJson(doc, json);
    ws.textAll(json);
    
    // MQTT Publish
    if (mqttClient.connected()) {
        mqttClient.publish("kiln/status", json.c_str());
    }
    
    // BLE Update
    if (deviceConnected && pStatusCharacteristic) {
        pStatusCharacteristic->setValue(json.c_str());
        pStatusCharacteristic->notify();
    }
}

// --- API & WiFi ---
void setupWiFi() {
    WiFiManager wm;
    // wm.resetSettings(); // Uncomment to wipe settings for testing
    
    bool res = wm.autoConnect("Kiln_Controller_Setup", "kiln1234");
    if(!res) {
        Serial.println("Failed to connect");
        // ESP.restart();
    } 
    else {
        Serial.println("connected...yeey :)");
    }
}

void setupMQTT() {
    mqttClient.setServer(mqtt_server, 1883);
}

void handleMQTT() {
    if (!mqttClient.connected()) {
        // Non-blocking reconnect attempt could go here
        // For simplicity, we just skip or do a quick check
        static unsigned long lastReconnect = 0;
        if (millis() - lastReconnect > 5000) {
            lastReconnect = millis();
            if (mqttClient.connect("KilnController")) {
                mqttClient.subscribe("kiln/command");
            }
        }
    }
    mqttClient.loop();
}

class ServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer) {
        deviceConnected = true;
    };
    void onDisconnect(NimBLEServer* pServer) {
        deviceConnected = false;
    }
};

void setupBLE() {
    NimBLEDevice::init("Kiln Controller");
    NimBLEServer *pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());
    
    NimBLEService *pService = pServer->createService("ABCD");
    pStatusCharacteristic = pService->createCharacteristic(
        "1234",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    
    pService->start();
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID("ABCD");
    pAdvertising->start();
    Serial.println("BLE Advertising...");
}

void setupAPI() {
    // Serve Static Files
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // Start Firing
    server.on("/api/start", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        DynamicJsonDocument doc(2048);
        deserializeJson(doc, data);
        
        std::vector<Step> newSchedule;
        JsonArray steps = doc["steps"];
        for (JsonVariant v : steps) {
            Step s;
            s.type = v["type"] | 0; // 0: Ramp, 1: Hold
            s.rate = v["rate"] | 0;
            s.target = v["target"] | 0;
            s.holdTime = v["hold"] | 0;
            newSchedule.push_back(s);
        }
        
        startFiring(newSchedule, doc["name"] | "Custom");
        request->send(200, "application/json", "{\"status\":\"started\"}");
    });

    // Stop Firing
    server.on("/api/stop", HTTP_POST, [](AsyncWebServerRequest *request){
        stopFiring();
        request->send(200, "application/json", "{\"status\":\"stopped\"}");
    });

    // --- Live Control API ---
    
    // 1. Skip Step
    server.on("/api/skip", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!state.isFiring) {
            request->send(400, "application/json", "{\"error\":\"Not firing\"}");
            return;
        }
        state.currentStepIndex++;
        state.stepStartTime = millis();
        
        if (state.currentStepIndex >= activeSchedule.size()) {
            state.status = COMPLETE;
            stopFiring();
            request->send(200, "application/json", "{\"status\":\"completed\"}");
        } else {
            request->send(200, "application/json", "{\"status\":\"skipped\", \"newStep\":" + String(state.currentStepIndex) + "}");
        }
    });

    // 2. Add Hold Time (or subtract)
    server.on("/api/hold", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!state.isFiring) {
            request->send(400, "application/json", "{\"error\":\"Not firing\"}");
            return;
        }
        
        // Only applicable if current step is HOLD? 
        // Or we can extend the current step regardless of type by shifting start time.
        // Let's shift start time: adding 5 mins means pretending we started 5 mins LATER.
        
        if (request->hasParam("minutes")) {
            int mins = request->getParam("minutes")->value().toInt();
            // If mins > 0, we extend hold (move start time forward? No, move it backward makes us think we hold longer? Wait.)
            // If we want to hold LONGER, we need to increase the step duration.
            // But step duration is fixed in activeSchedule. 
            // Better approach: Modify the active schedule in RAM.
            
            Step& currentStep = activeSchedule[state.currentStepIndex];
            if (currentStep.type == 1) { // Hold
                currentStep.holdTime += mins;
                request->send(200, "application/json", "{\"status\":\"updated\", \"newHoldTime\":" + String(currentStep.holdTime) + "}");
            } else {
                request->send(400, "application/json", "{\"error\":\"Current step is not HOLD\"}");
            }
        } else {
            request->send(400, "application/json", "{\"error\":\"Missing minutes param\"}");
        }
    });

    // 3. Change Setpoint (Override)
    server.on("/api/setpoint", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!state.isFiring) {
            request->send(400, "application/json", "{\"error\":\"Not firing\"}");
            return;
        }
        
        if (request->hasParam("temp")) {
            float newTemp = request->getParam("temp")->value().toFloat();
            // Modify current step target
            activeSchedule[state.currentStepIndex].target = newTemp;
            
            // If in HOLD, update setpoint immediately
            if (state.status == HOLD) {
                Setpoint[0] = newTemp;
            }
            // If in RAMP, the ramp logic will naturally aim for this new target
            
            request->send(200, "application/json", "{\"status\":\"updated\", \"newTarget\":" + String(newTemp) + "}");
        } else {
            request->send(400, "application/json", "{\"error\":\"Missing temp param\"}");
         }
     });
     
    // 4. PID AutoTune
    server.on("/api/autotune", HTTP_POST, [](AsyncWebServerRequest *request){
        if (state.isFiring || state.isTuning) {
            request->send(400, "application/json", "{\"error\":\"Busy\"}");
            return;
        }
        
        state.isTuning = true;
        state.tuneStep = 0;
        state.status = TUNING;
        state.tuneTarget = 150.0; // Default
        
        if (request->hasParam("temp")) {
            state.tuneTarget = request->getParam("temp")->value().toFloat();
        }
        
        request->send(200, "application/json", "{\"status\":\"started\", \"target\":" + String(state.tuneTarget) + "}");
    });
     
    // Get Status
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
        DynamicJsonDocument doc(1024);
        doc["temp"] = state.currentTemp[0];
        doc["target"] = state.targetTemp;
        doc["status"] = statusStr[state.status];
        doc["output"] = Output[0];
        doc["step"] = state.currentStepIndex + 1;
        doc["totalSteps"] = activeSchedule.size();
        
        // Time Info
        struct tm timeinfo;
        if(getLocalTime(&timeinfo)){
            char timeString[64];
            strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
            doc["time"] = String(timeString);
        } else {
            doc["time"] = "NTP Syncing...";
        }

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // Save Settings API
    server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        DynamicJsonDocument doc(512);
        deserializeJson(doc, (const char*)data);
        
        // Update Variables
        if (doc.containsKey("kp")) Kp = doc["kp"];
        if (doc.containsKey("ki")) Ki = doc["ki"];
        if (doc.containsKey("kd")) Kd = doc["kd"];
        
        // Save to NVS
        preferences.begin("kiln-config", false);
        if (doc.containsKey("kp")) preferences.putDouble("kp", Kp);
        if (doc.containsKey("ki")) preferences.putDouble("ki", Ki);
        if (doc.containsKey("kd")) preferences.putDouble("kd", Kd);
        if (doc.containsKey("offset")) {
            int offset = doc["offset"];
            preferences.putInt("offset", offset);
        }
        preferences.end();
        
        // Update PID
        pid0.SetTunings(Kp, Ki, Kd);
        
        request->send(200, "application/json", "{\"status\":\"saved\"}");
    });

    ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len){
        if(type == WS_EVT_CONNECT){
            Serial.printf("ws[%s][%u] connect\n", server->url(), client->id());
        }
    });
    server.addHandler(&ws);
    server.begin();
}

void loadConfig() {
    // Placeholder for loading PID values from LittleFS
}

void saveConfig() {
    // Placeholder for saving PID values to LittleFS
}
