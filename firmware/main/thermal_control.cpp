#include "thermal_control.h"
#include <string>
#include <vector>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include <cmath>

static const char *TAG = "THERMAL";

volatile uint16_t g_raw_value = 0;


// PID Defaults
double Kp = 22.2, Ki = 1.06, Kd = 116.55;

ThermalController::ThermalController() {
    mutex = NULL;
    
    state.status = KILN_IDLE;
    state.isFiring = false;
    state.currentTemp = 20.0;
    state.targetTemp = 0.0;
    state.currentStep = 0;
    state.totalSteps = 0;
    state.errorMsg = "";
}

ThermalController::~ThermalController() {
    if (thermocouple) {
        delete thermocouple;
        thermocouple = nullptr;
    }
    if (pid) {
        delete pid;
        pid = nullptr;
    }
    if (mutex) {
        vSemaphoreDelete(mutex);
        mutex = nullptr;
    }
}

void ThermalController::begin() {
    if (mutex == NULL) {
        mutex = xSemaphoreCreateRecursiveMutex();
    }

    // Force safe initial state
    state.status = KILN_IDLE;
    state.isFiring = false;
    state.currentStep = 0;
    state.totalSteps = 0;
    activeSchedule.clear();

    // Hardware Pins
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << SSR_ZONE1_PIN) | (1ULL << SAFETY_RELAY_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    gpio_set_level((gpio_num_t)SSR_ZONE1_PIN, 0);
    gpio_set_level((gpio_num_t)SAFETY_RELAY_PIN, 0);

    // Thermocouple
    thermocouple = new MAX6675(MAXCS, MAXCLK, MAXDO);
    
    // PID
    pid = new PID(&input, &output, &setpoint, Kp, Ki, Kd, DIRECT);
    pid->SetOutputLimits(0, PID_WINDOW_SIZE); // Window Size 2000ms
    pid->SetMode(AUTOMATIC);
    windowStartTime = esp_timer_get_time() / 1000;

    ESP_LOGI(TAG, "Thermal Controller initialized");
}

void ThermalController::loop() {
    if (!mutex) return;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    
    updateTemperature();

    if (!isSensorHealthy()) {
        ESP_LOGE(TAG, "Sensor is not healthy, initiating emergency stop.");
        emergencyStop("Sensor Failure");
        xSemaphoreGiveRecursive(mutex);
        return;
    }

    if (state.isFiring) {
        processSchedule();
        computePID();
        driveSSR();
        

    } else {
        gpio_set_level((gpio_num_t)SSR_ZONE1_PIN, 0);
        gpio_set_level((gpio_num_t)SAFETY_RELAY_PIN, 0);
        output = 0;
        setpoint = 0;
    }
    
    xSemaphoreGiveRecursive(mutex);
}

void ThermalController::updateTemperature() {
    if (!thermocouple) {
        ESP_LOGE(TAG, "Thermocouple not initialized!");
        return;
    }
    float t = thermocouple->readCelsius();
    ESP_LOGI(TAG, "Raw Sensor Value: %u, Temperature: %.2f C", g_raw_value, t);
    if (!std::isnan(t)) {
        state.currentTemp = t;
        ESP_LOGI(TAG, "Current Temperature: %.2f C", state.currentTemp);
    } else {
        ESP_LOGW(TAG, "Thermocouple reading is NaN. Sensor might be disconnected or faulty.");
        if (state.isFiring) {
            emergencyStop("Sensor Disconnected (NaN)");
        }
    }
}

bool ThermalController::isSensorHealthy() {
    if (std::isnan(state.currentTemp) || state.currentTemp > MAX_TEMP_SAFETY || state.currentTemp < -50) {
        return false;
    }
    return true;
}

void ThermalController::processSchedule() {
    if (activeSchedule.empty()) {
        stop("Empty Schedule");
        state.status = KILN_IDLE;
        return;
    }

    if (state.currentStep >= activeSchedule.size()) {
        stop("Schedule Complete");
        state.status = KILN_COMPLETE;
        return;
    }

    uint64_t now = esp_timer_get_time() / 1000;
    uint64_t elapsed = (now - stepStartTime) / 1000; // seconds
    ScheduleStep step = activeSchedule[state.currentStep];
    ESP_LOGI(TAG, "Processing Step %d of %d: Type %d, Value %.2f", state.currentStep + 1, state.totalSteps, step.type, step.value);

    if (step.type == 0) { // RAMP
        state.status = KILN_RAMP;
        state.targetTemp = step.value;
        setpoint = state.targetTemp;
        
        if (state.currentTemp >= step.value - 1.0f) { // Hysteresis 1.0C
            state.currentStep++;
            ESP_LOGI(TAG, "RAMP Step %d completed. Moving to next step.", state.currentStep);
            stepStartTime = esp_timer_get_time() / 1000;
            stepStartTemp = state.currentTemp;
        }

    } else if (step.type == 1) { // HOLD
        state.status = KILN_HOLD;
        setpoint = state.targetTemp; // Keep previous target
        if (elapsed >= (uint32_t)(step.value * 60)) {
            state.currentStep++;
            ESP_LOGI(TAG, "HOLD Step %d completed. Moving to next step.", state.currentStep);
            stepStartTime = esp_timer_get_time() / 1000;
            stepStartTemp = state.currentTemp;
        }
    }
}

void ThermalController::computePID() {
    input = state.currentTemp;
    pid->Compute();
}

void ThermalController::driveSSR() {
    uint64_t now = esp_timer_get_time() / 1000;
    if (now - windowStartTime > PID_WINDOW_SIZE) {
        windowStartTime += PID_WINDOW_SIZE;
    }
    
    if (output > (now - windowStartTime)) {
        gpio_set_level((gpio_num_t)SSR_ZONE1_PIN, 1);
    } else {
        gpio_set_level((gpio_num_t)SSR_ZONE1_PIN, 0);
    }
}

void ThermalController::startSchedule(const std::string& scheduleJson) {
    loadSchedule(scheduleJson);
    // REMOVED: start(); - This was causing auto-start when using old API calls
    // Now startSchedule just loads. Explicit start() call needed.
}

void ThermalController::loadSchedule(const std::string& scheduleJson) {
    if (!mutex) return;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    
    ESP_LOGI(TAG, "Received schedule JSON: %s", scheduleJson.c_str());

    cJSON *root = cJSON_Parse(scheduleJson.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse schedule JSON");
        state.errorMsg = "Invalid JSON";
        xSemaphoreGiveRecursive(mutex);
        return;
    }
    
    // Clear old schedule
    activeSchedule.clear();
    
    // Populate new schedule
    cJSON *steps = cJSON_GetObjectItem(root, "steps");
    if (!cJSON_IsArray(steps)) {
        ESP_LOGE(TAG, "Schedule JSON missing 'steps' array");
        state.errorMsg = "Invalid Schedule";
        cJSON_Delete(root);
        xSemaphoreGiveRecursive(mutex);
        return;
    }

    cJSON *step = NULL;
    cJSON_ArrayForEach(step, steps) {
        if (!cJSON_IsObject(step)) continue;

        ScheduleStep s;
        const cJSON *typeItem = cJSON_GetObjectItem(step, "type");
        std::string type = (cJSON_IsString(typeItem) && typeItem->valuestring) ? typeItem->valuestring : "ramp";
        s.type = (type == "hold") ? 1 : 0;
        
        if (s.type == 0) { // RAMP
            const cJSON *targetItem = cJSON_GetObjectItem(step, "target");
            s.value = cJSON_IsNumber(targetItem) ? (float)targetItem->valuedouble : 0.0f;
        } else { // HOLD
            const cJSON *holdTimeItem = cJSON_GetObjectItem(step, "holdTime");
            s.value = cJSON_IsNumber(holdTimeItem) ? (float)holdTimeItem->valuedouble : 0.0f; // Using 'holdTime' for HOLD
        }
        activeSchedule.push_back(s);
    }

    cJSON_Delete(root);
    
    if (activeSchedule.empty()) {
        state.errorMsg = "Empty Schedule";
        xSemaphoreGiveRecursive(mutex);
        return;
    }
    
    // Just load, do not start
    state.isFiring = false;
    state.status = KILN_IDLE;
    state.currentStep = 0;
    state.totalSteps = activeSchedule.size();
    state.errorMsg = "";
    
    if (activeSchedule.empty()) {
        ESP_LOGE(TAG, "Schedule is empty after loading!");
    }
    ESP_LOGI(TAG, "Schedule Loaded with %d steps", state.totalSteps);
    
    xSemaphoreGiveRecursive(mutex);
}

void ThermalController::start() {
    if (!mutex) return;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    
    if (activeSchedule.empty()) {
        state.errorMsg = "No Schedule Loaded";
        xSemaphoreGiveRecursive(mutex);
        return;
    }
    
    // Reset State
    state.isFiring = true;
    state.status = KILN_RAMP;
    state.currentStep = 0;
    state.errorMsg = "";
    stepStartTime = esp_timer_get_time() / 1000;
    stepStartTemp = state.currentTemp;
    windowStartTime = esp_timer_get_time() / 1000;
    
    // Enable Safety Relay
    gpio_set_level((gpio_num_t)SAFETY_RELAY_PIN, 1);
    
    ESP_LOGI(TAG, "Schedule Started");
    
    xSemaphoreGiveRecursive(mutex);
}

void ThermalController::stop(const std::string& reason) {
    if (!mutex) return;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    
    state.isFiring = false;
    state.status = KILN_IDLE;
    state.errorMsg = reason;
    
    // Immediately cut power
    gpio_set_level((gpio_num_t)SSR_ZONE1_PIN, 0);
    gpio_set_level((gpio_num_t)SAFETY_RELAY_PIN, 0);
    output = 0;
    setpoint = 0;
    state.targetTemp = 0;
    
    ESP_LOGI(TAG, "KILN STOPPED: %s", reason.c_str());
    
    xSemaphoreGiveRecursive(mutex);
}

void ThermalController::emergencyStop(const std::string& reason) {
    if (!mutex) return;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);

    // Explicitly call stop logic inline to avoid recursion deadlock if stop() also takes mutex
    // But since it's recursive mutex, it should be fine.
    // However, let's just duplicate the shutdown logic to be absolutely safe and fast.

    state.isFiring = false;
    state.status = KILN_ERROR;
    state.errorMsg = reason;

    gpio_set_level((gpio_num_t)SSR_ZONE1_PIN, 0);
    gpio_set_level((gpio_num_t)SAFETY_RELAY_PIN, 0);
    output = 0;
    setpoint = 0;
    state.targetTemp = 0;

    ESP_LOGE(TAG, "EMERGENCY STOP: %s", reason.c_str());

    xSemaphoreGiveRecursive(mutex);
}

void ThermalController::skipCurrentStep() {
    // Ensure thread safety
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);

    if (state.isFiring) {
        ESP_LOGI(TAG, "Skipping current step. Current step: %d, Total steps: %d", state.currentStep + 1, state.totalSteps);
        state.currentStep++; // Move to next step

        if (state.currentStep >= state.totalSteps) {
            stop("Schedule Complete (Skipped)");
        } else {
            // Reset step start time and temperature for the new step
            stepStartTime = esp_timer_get_time() / 1000;
            stepStartTemp = state.currentTemp;

            // Update target temperature based on the new step
            ScheduleStep nextStep = activeSchedule[state.currentStep];
            if (nextStep.type == 0) { // RAMP
                state.targetTemp = nextStep.value;
            } else { // HOLD
                state.targetTemp = state.currentTemp; // For HOLD, target is current temp
            }
            ESP_LOGI(TAG, "Moved to Step %d. New target temp: %.2f", state.currentStep + 1, state.targetTemp);
        }
    } else {
        ESP_LOGW(TAG, "Attempted to skip step but kiln is not firing.");
    }

    xSemaphoreGiveRecursive(mutex);
}

void ThermalController::addTimeToHold(float minutesToAdd) {
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);

    if (state.isFiring && state.status == KILN_HOLD && state.currentStep < activeSchedule.size()) {
        ScheduleStep currentStep = activeSchedule[state.currentStep];
        if (currentStep.type == 1) { // Only add time if it's a HOLD step
            activeSchedule[state.currentStep].value += minutesToAdd;
            ESP_LOGI(TAG, "Added %.2f minutes to current HOLD step. New hold time: %.2f min", minutesToAdd, activeSchedule[state.currentStep].value);
        } else {
            ESP_LOGW(TAG, "Attempted to add time, but current step is not a HOLD step.");
        }
    } else {
        ESP_LOGW(TAG, "Attempted to add time but kiln is not firing or not in HOLD state.");
    }

    xSemaphoreGiveRecursive(mutex);
}

void ThermalController::addTemperatureToTarget(float tempToAdd) {
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);

    if (state.isFiring) {
        state.targetTemp += tempToAdd;
        setpoint = state.targetTemp; // Update PID setpoint immediately
        ESP_LOGI(TAG, "Added %.2f C to target. New target temp: %.2f C", tempToAdd, state.targetTemp);
    } else {
        ESP_LOGW(TAG, "Attempted to add temperature but kiln is not firing.");
    }

    xSemaphoreGiveRecursive(mutex);
}



KilnState ThermalController::getState() {
    if (mutex) xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    KilnState s = state;
    if (mutex) xSemaphoreGiveRecursive(mutex);
    return s;
}

double ThermalController::getOutput() {
    if (mutex) xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    double o = output;
    if (mutex) xSemaphoreGiveRecursive(mutex);
    return o;
}

ThermalController thermalCtrl;
