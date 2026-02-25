#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_MAX31855.h>
#include <PID_v1.h>
#include "display_gui.h"

// --- Configuration ---
#define SSR_PIN 15
#define MAXDO 19
#define MAXCS 5
#define MAXCLK 18
#define WIFI_SSID "Kiln_Network"
#define WIFI_PASS "kiln1234"

// --- Objects ---
Adafruit_MAX31855 thermocouple(MAXCLK, MAXCS, MAXDO);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// --- PID Variables ---
double Setpoint, Input, Output;
double Kp = 200, Ki = 5, Kd = 10; // Needs tuning
PID myPID(&Input, &Output, &Setpoint, Kp, Ki, Kd, DIRECT);

// --- State ---
struct KilnState {
    bool isFiring = false;
    float currentTemp = 0.0;
    float targetTemp = 0.0;
    String status = "IDLE"; // IDLE, RAMP, HOLD, COOL, ERROR
    unsigned long startTime = 0;
    int currentStep = 0;
    String errorMsg = "";
} state;

// --- Schedule Model ---
struct Step {
    int type; // 0: Ramp, 1: Hold
    float target;
    int duration; // minutes
};
std::vector<Step> activeSchedule;

// --- Function Prototypes ---
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
void runControlLoop();
void updateSafety();

void setup() {
    Serial.begin(115200);
    
    // Hardware Init
    pinMode(SSR_PIN, OUTPUT);
    digitalWrite(SSR_PIN, LOW);
    
    if (!thermocouple.begin()) {
        state.errorMsg = "Thermocouple Error!";
        Serial.println("ERROR: MAX31855 not found.");
    }

    // Display Init
    setupDisplay();

    // PID Init
    myPID.SetMode(AUTOMATIC);
    myPID.SetOutputLimits(0, 1000); // Window size 1000ms

    // WiFi
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    // In production, use WiFiManager or AP mode fallback
    
    // API & WebSocket
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{";
        json += "\"temp\":" + String(state.currentTemp) + ",";
        json += "\"target\":" + String(state.targetTemp) + ",";
        json += "\"state\":\"" + state.status + "\"";
        json += "}";
        request->send(200, "application/json", json);
    });

    server.on("/api/start", HTTP_POST, [](AsyncWebServerRequest *request){
        // Parse schedule from body (simplified here)
        state.isFiring = true;
        state.startTime = millis();
        state.status = "RAMP";
        request->send(200, "text/plain", "Started");
    });

    server.on("/api/stop", HTTP_POST, [](AsyncWebServerRequest *request){
        state.isFiring = false;
        state.status = "IDLE";
        digitalWrite(SSR_PIN, LOW);
        request->send(200, "text/plain", "Stopped");
    });

    server.begin();
}

void loop() {
    // 1. Read Sensors
    double c = thermocouple.readCelsius();
    if (isnan(c)) {
        state.errorMsg = "Sensor Fault";
        state.isFiring = false; // Safety shutoff
    } else {
        state.currentTemp = c;
        Input = state.currentTemp;
    }

    // 2. Logic & Control
    if (state.isFiring) {
        runControlLoop();
    } else {
        digitalWrite(SSR_PIN, LOW);
        state.targetTemp = 0;
    }

    // 3. Update UI
    updateDisplay(state.currentTemp, state.targetTemp, state.status, state.isFiring);

    // 4. Telemetry
    static unsigned long lastTelemetry = 0;
    if (millis() - lastTelemetry > 1000) {
        String msg = "{\"temp\":" + String(state.currentTemp) + "}";
        ws.textAll(msg);
        lastTelemetry = millis();
    }
    
    ws.cleanupClients();
}

void runControlLoop() {
    // Simple Ramp/Hold Logic execution would go here
    // For this example, we assume a single setpoint hold
    // In real implementation: calculate Setpoint based on time and ramp rate
    
    // PID Calculation
    myPID.Compute();

    // Time Proportional Output for SSR
    static unsigned long windowStartTime = 0;
    if (millis() - windowStartTime > 1000) {
        windowStartTime = millis();
    }
    if (Output > (millis() - windowStartTime)) digitalWrite(SSR_PIN, HIGH);
    else digitalWrite(SSR_PIN, LOW);
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if(type == WS_EVT_CONNECT){
        Serial.println("Websocket Client Connected");
    }
}