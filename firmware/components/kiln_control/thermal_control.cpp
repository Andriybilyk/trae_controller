#include "kiln_control/thermal_control.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "kiln_config/config.h"
#include "kiln_config/config_store.h"
#include "kiln_config/fs_utils.h"
#include "kiln_hal_heater/heater_hal.h"
#include "kiln_control/PID.h"
#include "kiln_control/max31855.h"
#include "kiln_safety/faults.h"

static const char *TAG = "THERMAL";
static constexpr uint32_t SENSOR_INVALID_LATCH_COUNT = 3;
static constexpr uint32_t SENSOR_VALID_RECOVERY_COUNT = 12;
static constexpr uint32_t SENSOR_STUCK_LATCH_COUNT = 240; // ~60s at 250ms sample time
static constexpr uint32_t SENSOR_SHORT_LATCH_COUNT = 20;  // ~5s
static constexpr float SENSOR_STUCK_DELTA_C = 0.05f;
static constexpr uint64_t SENSOR_READ_TIMEOUT_MS = 5000;
static constexpr const char* RECOVERY_FILE = "/littlefs/recovery.json";
static constexpr const char* EVENTS_LOG_FILE = "/littlefs/logs/events.log";
static constexpr const char* EVENTS_LOG_PREV_FILE = "/littlefs/logs/events.log.1";
static constexpr size_t EVENTS_LOG_MAX_BYTES = 64 * 1024;

static void rotate_event_log_if_needed() {
    const size_t size = kiln_fs_file_size(EVENTS_LOG_FILE);
    if (size < EVENTS_LOG_MAX_BYTES) return;
    SemaphoreHandle_t m = kiln_config_fs_mutex();
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    (void)unlink(EVENTS_LOG_PREV_FILE);
    (void)rename(EVENTS_LOG_FILE, EVENTS_LOG_PREV_FILE);
    if (m) xSemaphoreGiveRecursive(m);
}

static void append_event_log(const char *type, const char *msg) {
    if (!type) type = "";
    if (!msg) msg = "";

    (void)mkdir("/littlefs/logs", 0777);
    rotate_event_log_if_needed();

    FILE *f = fopen(EVENTS_LOG_FILE, "a");
    if (!f) return;
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    fprintf(f, "%llu %s %s\n", (unsigned long long)now_ms, type, msg);
    fflush(f);
    const int fd = fileno(f);
    if (fd >= 0) (void)fsync(fd);
    fclose(f);
}


// PID Defaults
static constexpr double Kp = (double)PID_KP;
static constexpr double Ki = (double)PID_KI;
static constexpr double Kd = (double)PID_KD;

ThermalController::ThermalController() {
    mutex = NULL;
    thermocouple = nullptr;
    pid = nullptr;
    thermoLastRaw = 0;
    thermoReadCount = 0;
    lastThermoReadMs = 0;
    lastSensorOk = false;
    sensorValidStreak = 0;
    sensorInvalidStreak = 0;
    sensorStuckStreak = 0;
    sensorShortStreak = 0;
    lastUpdate = 0;
    lastTemp = 0.0f;
    lastTempValid = false;
    sensorHealthySinceMs = 0;
    lastSensorHealthy = false;
    tempFilterCount = 0;
    tempFilterIndex = 0;
    runtimeKp = Kp;
    runtimeKi = Ki;
    runtimeKd = Kd;
    temperatureOffsetC = 0.0f;
    userMaxTempC = (float)MAX_TEMP_SAFETY;
    autotuneTargetC = 600.0f;

    loadedScheduleName.clear();

    historyActive = false;
    historyId.clear();
    historyStartMs = 0;
    historyPeakTemp = 0.0f;
    lastHistorySampleMs = 0;
    historyPoints.clear();
    
    state.status = KILN_IDLE;
    state.isFiring = false;
    state.currentTemp = 20.0;
    state.targetTemp = 0.0;
    state.startTime = 0;
    state.duration = 0;
    state.timeRemaining = -1;
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
    thermoLastRaw = 0;
    thermoReadCount = 0;
    lastThermoReadMs = 0;
    lastSensorOk = false;
    sensorValidStreak = 0;
    sensorInvalidStreak = 0;
    sensorStuckStreak = 0;
    sensorShortStreak = 0;
    lastUpdate = 0;
    lastTempValid = false;
    sensorHealthySinceMs = 0;
    lastSensorHealthy = false;
    tempFilterCount = 0;
    tempFilterIndex = 0;

    loadedScheduleName.clear();
    historyActive = false;
    historyId.clear();
    historyStartMs = 0;
    historyPeakTemp = 0.0f;
    lastHistorySampleMs = 0;
    historyPoints.clear();

    heater_hal_init();
    heater_hal_all_off();
    kiln_fault_clear();

    thermocouple = new MAX31855(MAXCLK, MAXCS, MAXDO);
    
    // PID
    pid = new PID(&input, &output, &setpoint, Kp, Ki, Kd, DIRECT);
    pid->SetOutputLimits(0, PID_WINDOW_SIZE); // Window Size 2000ms
    pid->SetMode(AUTOMATIC);
    windowStartTime = esp_timer_get_time() / 1000;

    // Load persisted settings (offset + PID tunings)
    loadRuntimeSettingsLocked();
    pid->SetTunings(runtimeKp, runtimeKi, runtimeKd);

    // Try to load recovery state
    loadRecoveryStateLocked();

    ESP_LOGI(TAG, "Thermal Controller initialized");
}

void ThermalController::tripFaultLocked(kiln_fault_code_t code, const char *reason) {
    if (!reason) reason = "";
    kiln_fault_set(code, reason);
    kiln_fault_update_reason(reason);
    applyFaultStateLocked();
}

void ThermalController::applyFaultStateLocked() {
    heater_hal_all_off();
    const bool wasFiring = state.isFiring;
    state.isFiring = false;
    state.status = KILN_FAULT;
    state.timeRemaining = -1;
    output = 0;
    setpoint = 0;
    state.targetTemp = 0;

    clearRecoveryStateLocked();

    const kiln_fault_t f = kiln_fault_get();
    state.errorMsg = f.reason;
    if (wasFiring && historyActive) {
        historyFinalizeLocked((uint64_t)(esp_timer_get_time() / 1000ULL), "ERROR");
    }
}

bool ThermalController::sensorRecoveredForClearLocked(uint64_t now_ms) const {
    return lastSensorOk &&
           sensorValidStreak >= SENSOR_VALID_RECOVERY_COUNT &&
           sensorHealthySinceMs > 0 &&
           (now_ms - sensorHealthySinceMs) >= SENSOR_FAULT_AUTO_CLEAR_MS &&
           state.currentTemp <= FAULT_CLEAR_MAX_TEMP_C;
}

void ThermalController::loop() {
    if (!mutex) return;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    
    updateTemperature();
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

    if (lastSensorOk) {
        if (!lastSensorHealthy) sensorHealthySinceMs = now_ms;
    } else {
        sensorHealthySinceMs = 0;
    }
    lastSensorHealthy = lastSensorOk;

    if (state.isFiring && lastThermoReadMs > 0 && (now_ms - lastThermoReadMs) > SENSOR_READ_TIMEOUT_MS) {
        tripFaultLocked(KILN_FAULT_WATCHDOG, "Thermocouple read timeout");
    }
    if (!kiln_fault_is_active() && std::isfinite(state.currentTemp) && state.currentTemp > MAX_TEMP_SAFETY) {
        tripFaultLocked(KILN_FAULT_OVERTEMP, "Overtemperature");
    }

    if (kiln_fault_is_active()) {
        const kiln_fault_t f = kiln_fault_get();
        const bool is_recoverable_fault =
            (f.code == KILN_FAULT_SENSOR_NAN || f.code == KILN_FAULT_SENSOR_OOR ||
             f.code == KILN_FAULT_SENSOR_OPEN || f.code == KILN_FAULT_SENSOR_SHORT ||
             f.code == KILN_FAULT_SENSOR_STUCK || f.code == KILN_FAULT_WATCHDOG);
        const bool can_clear = is_recoverable_fault && sensorRecoveredForClearLocked(now_ms);
        if (is_recoverable_fault && can_clear) {
            ESP_LOGI(TAG, "Auto-clearing sensor fault after recovery.");
            kiln_fault_clear();
            state.isFiring = false;
            state.status = KILN_IDLE;
            state.errorMsg = "";
            state.duration = 0;
            state.timeRemaining = -1;
            output = 0;
            setpoint = 0;
            state.targetTemp = 0;
            sensorInvalidStreak = 0;
            sensorStuckStreak = 0;
            sensorShortStreak = 0;
            sensorValidStreak = 0;
            xSemaphoreGiveRecursive(mutex);
            return;
        }
        applyFaultStateLocked();
        xSemaphoreGiveRecursive(mutex);
        return;
    }

    if (state.isFiring && state.startTime) {
        const uint64_t now_s = (uint64_t)(esp_timer_get_time() / 1000000ULL);
        state.duration = (uint32_t)(now_s - state.startTime);
    } else {
        state.duration = 0;
    }

    if (tune.active) {
        autotuneLoopLocked(now_ms);
        xSemaphoreGiveRecursive(mutex);
        return;
    }

    if (state.isFiring) {
        processSchedule();
        computePID();
        driveSSR();
        historySampleLocked(now_ms);
        
        // Save recovery state every 1 minute
        static uint64_t last_recovery_save = 0;
        if (now_ms - last_recovery_save > 60000) {
            saveRecoveryStateLocked();
            last_recovery_save = now_ms;
        }

    } else {
        heater_hal_all_off();
        output = 0;
        setpoint = 0;
        state.timeRemaining = -1;
    }
    
    xSemaphoreGiveRecursive(mutex);
}

void ThermalController::updateTemperature() {
    if (!thermocouple) {
        ESP_LOGE(TAG, "Thermocouple not initialized!");
        return;
    }

    // Thermocouple conversion guard.
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    if (lastThermoReadMs != 0 && (now_ms - lastThermoReadMs) < 250) {
        return;
    }
    lastThermoReadMs = now_ms;

    uint32_t raw = 0;
    float t = thermocouple->readCelsius(&raw);
    thermoLastRaw = raw;
    thermoReadCount++;
    const bool raw_all_zero = (raw == 0x00000000U);
    const bool raw_all_ones = (raw == 0xFFFFFFFFU);
    const bool raw_comm_bad = raw_all_zero || raw_all_ones;
    if (raw_comm_bad) t = NAN;

    const bool raw_fault = (raw & 0x00010000U) != 0;
    const bool raw_open = raw_fault && ((raw & 0x00000001U) != 0);
    const bool raw_short = raw_fault && ((raw & 0x00000006U) != 0);

    if (raw_comm_bad || raw_fault) {
        static uint64_t last_log_ms = 0;
        if (now_ms - last_log_ms >= 1000) {
            ESP_LOGW(TAG,
                     "Thermo raw=0x%08" PRIx32 " comm_bad=%d fault=%d open=%d short=%d",
                     raw,
                     (int)raw_comm_bad,
                     (int)raw_fault,
                     (int)raw_open,
                     (int)raw_short);
            last_log_ms = now_ms;
        }
    }

    const float measured = t + temperatureOffsetC;
    const bool finite = std::isfinite(measured);
    const bool in_range = finite && measured >= SENSOR_TEMP_MIN_C && measured <= SENSOR_TEMP_MAX_C;

    if ((!finite || !in_range) && !raw_fault && !raw_comm_bad) {
        static uint64_t last_bad_log_ms = 0;
        if (now_ms - last_bad_log_ms >= 1000) {
            ESP_LOGW(TAG,
                     "Thermo invalid raw=0x%08" PRIx32 " t=%.2f off=%.2f measured=%.2f finite=%d in_range=%d",
                     raw,
                     (double)t,
                     (double)temperatureOffsetC,
                     (double)measured,
                     (int)finite,
                     (int)in_range);
            last_bad_log_ms = now_ms;
        }
    }

    if (raw_fault || !finite || !in_range) {
        sensorValidStreak = 0;
        sensorInvalidStreak++;
        lastSensorOk = false;
        lastTempValid = false;
        state.currentTemp = NAN;

        if (raw_comm_bad && sensorInvalidStreak >= SENSOR_INVALID_LATCH_COUNT) {
            tripFaultLocked(KILN_FAULT_SENSOR_NAN, "Sensor comm/no-data");
        } else if (raw_open && sensorInvalidStreak >= SENSOR_INVALID_LATCH_COUNT) {
            tripFaultLocked(KILN_FAULT_SENSOR_OPEN, "Sensor open");
        } else if (raw_short && sensorInvalidStreak >= SENSOR_INVALID_LATCH_COUNT) {
            tripFaultLocked(KILN_FAULT_SENSOR_SHORT, "Sensor short");
        } else if (!finite && sensorInvalidStreak >= SENSOR_INVALID_LATCH_COUNT) {
            tripFaultLocked(KILN_FAULT_SENSOR_NAN, "Sensor NaN/open");
        } else if (sensorInvalidStreak >= SENSOR_INVALID_LATCH_COUNT) {
            tripFaultLocked(KILN_FAULT_SENSOR_OOR, "Sensor out of range");
        }
        return;
    }

    float filtered = measured;
    {
        const float previous = lastTemp;
        const bool have_previous = lastTempValid;

        if (tempFilterCount < 5) {
            tempFilterBuf[tempFilterIndex] = measured;
            tempFilterIndex = (uint8_t)((tempFilterIndex + 1) % 5);
            tempFilterCount++;
        } else {
            tempFilterBuf[tempFilterIndex] = measured;
            tempFilterIndex = (uint8_t)((tempFilterIndex + 1) % 5);
        }

        float tmp[5];
        const uint8_t n = tempFilterCount;
        for (uint8_t i = 0; i < n; i++) tmp[i] = tempFilterBuf[i];
        std::sort(tmp, tmp + n);
        filtered = tmp[n / 2];

        if (have_previous && lastUpdate != 0 && now_ms > lastUpdate) {
            const float dt_min = (float)(now_ms - lastUpdate) / 60000.0f;
            const float max_jump = std::max(5.0f, MAX_TEMP_RISE_RATE * dt_min * 2.0f + 5.0f);
            if (std::fabs(filtered - previous) > max_jump) {
                static uint64_t last_spike_log_ms = 0;
                if (now_ms - last_spike_log_ms >= 1000) {
                    ESP_LOGW(TAG,
                             "Thermo spike raw=0x%08" PRIx32 " last=%.2f now=%.2f max_jump=%.2f",
                             raw,
                             (double)previous,
                             (double)filtered,
                             (double)max_jump);
                    last_spike_log_ms = now_ms;
                }
                sensorValidStreak = 0;
                sensorInvalidStreak++;
                lastSensorOk = false;
                if (sensorInvalidStreak >= SENSOR_INVALID_LATCH_COUNT) {
                    tripFaultLocked(KILN_FAULT_SENSOR_OOR, "Sensor spike/out of range");
                }
                return;
            }
        }
    }

    sensorInvalidStreak = 0;
    sensorValidStreak++;
    lastSensorOk = true;
    state.currentTemp = filtered;

    {
        static uint64_t last_ok_log_ms = 0;
        if (now_ms - last_ok_log_ms >= 5000) {
            int32_t cj12 = (int32_t)((raw >> 4) & 0x0FFFU);
            if (cj12 & 0x0800) cj12 |= ~0x0FFF;
            const float cj_c = (float)cj12 * 0.0625f;
            ESP_LOGI(TAG,
                     "Thermo ok raw=0x%08" PRIx32 " measured=%.2f filtered=%.2f cj=%.2f",
                     raw,
                     (double)measured,
                     (double)filtered,
                     (double)cj_c);
            last_ok_log_ms = now_ms;
        }
    }

    // Rise-rate check (C/min) with valid points only.
    if (lastUpdate != 0 && now_ms > lastUpdate && lastTempValid) {
        const uint64_t dt_ms = now_ms - lastUpdate;
        if (dt_ms >= 1000) {
            const float dT = state.currentTemp - lastTemp;
            const float rate_c_per_min = dT * (60000.0f / (float)dt_ms);
            if (rate_c_per_min > MAX_TEMP_RISE_RATE) {
                tripFaultLocked(KILN_FAULT_RISE_RATE, "Rise rate exceeded");
            }
        }
    }

    sensorShortStreak = 0;

    // Stuck-value detection while heating demand is high.
    const bool demanding_heat = state.isFiring && std::isfinite(setpoint) && setpoint > state.currentTemp + 20.0f;
    if (demanding_heat && lastTempValid && std::fabs(state.currentTemp - lastTemp) <= SENSOR_STUCK_DELTA_C) {
        sensorStuckStreak++;
        if (sensorStuckStreak >= SENSOR_STUCK_LATCH_COUNT) {
            tripFaultLocked(KILN_FAULT_SENSOR_STUCK, "Sensor stuck");
        }
    } else {
        sensorStuckStreak = 0;
    }

    lastTemp = state.currentTemp;
    lastTempValid = true;
    lastUpdate = now_ms;
}

void ThermalController::loadRuntimeSettingsLocked() {
    runtimeKp = Kp;
    runtimeKi = Ki;
    runtimeKd = Kd;
    temperatureOffsetC = 0.0f;

    const std::string file = kiln_config_load_json_config();
    if (file.empty()) return;

    cJSON *root = cJSON_Parse(file.c_str());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return;
    }

    const cJSON *off = cJSON_GetObjectItem(root, "temp_offset_c");
    if (!cJSON_IsNumber(off)) off = cJSON_GetObjectItem(root, "offset");
    if (cJSON_IsNumber(off)) temperatureOffsetC = (float)off->valuedouble;

    const cJSON *maxC = cJSON_GetObjectItem(root, "maxC");
    if (cJSON_IsNumber(maxC)) {
        const float v = (float)maxC->valuedouble;
        userMaxTempC = std::max(100.0f, std::min((float)MAX_TEMP_SAFETY, v));
    }
    const cJSON *autotuneTarget = cJSON_GetObjectItem(root, "autotune_target_c");
    if (cJSON_IsNumber(autotuneTarget)) {
        const float v = (float)autotuneTarget->valuedouble;
        autotuneTargetC = std::max(100.0f, std::min(1200.0f, v));
    }

    const cJSON *pidObj = cJSON_GetObjectItem(root, "pid");
    if (cJSON_IsObject(pidObj)) {
        const cJSON *kp = cJSON_GetObjectItem(pidObj, "kp");
        const cJSON *ki = cJSON_GetObjectItem(pidObj, "ki");
        const cJSON *kd = cJSON_GetObjectItem(pidObj, "kd");
        if (cJSON_IsNumber(kp) && cJSON_IsNumber(ki) && cJSON_IsNumber(kd)) {
            runtimeKp = kp->valuedouble;
            runtimeKi = ki->valuedouble;
            runtimeKd = kd->valuedouble;
        }
    }

    cJSON_Delete(root);
}

void ThermalController::persistRuntimeSettingsLocked() {
    cJSON *root = nullptr;
    const std::string existing = kiln_config_load_json_config();
    if (!existing.empty()) root = cJSON_Parse(existing.c_str());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        root = cJSON_CreateObject();
    }

    cJSON_DeleteItemFromObject(root, "temp_offset_c");
    cJSON_AddNumberToObject(root, "temp_offset_c", (double)temperatureOffsetC);
    cJSON_DeleteItemFromObject(root, "autotune_target_c");
    cJSON_AddNumberToObject(root, "autotune_target_c", (double)autotuneTargetC);

    cJSON *pidObj = cJSON_GetObjectItem(root, "pid");
    if (!cJSON_IsObject(pidObj)) {
        cJSON_DeleteItemFromObject(root, "pid");
        pidObj = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "pid", pidObj);
    }
    cJSON_DeleteItemFromObject(pidObj, "kp");
    cJSON_DeleteItemFromObject(pidObj, "ki");
    cJSON_DeleteItemFromObject(pidObj, "kd");
    cJSON_AddNumberToObject(pidObj, "kp", runtimeKp);
    cJSON_AddNumberToObject(pidObj, "ki", runtimeKi);
    cJSON_AddNumberToObject(pidObj, "kd", runtimeKd);

    char *rendered = cJSON_PrintUnformatted(root);
    const std::string out = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(root);
    (void)kiln_config_save_json_config(out);
}

void ThermalController::saveRecoveryStateLocked() {
    if (!state.isFiring || activeSchedule.empty()) return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "currentStep", state.currentStep);
    cJSON_AddNumberToObject(root, "startTime", (double)state.startTime);
    cJSON_AddNumberToObject(root, "stepStartTime", (double)stepStartTime);
    cJSON_AddNumberToObject(root, "stepStartTemp", (double)stepStartTemp);
    
    // Save schedule JSON string so we can restore the exact schedule
    cJSON *schedArr = cJSON_CreateArray();
    for (const auto& step : activeSchedule) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "type", step.type == 0 ? "ramp" : "hold");
        if (step.type == 0) {
            cJSON_AddNumberToObject(s, "target", step.value);
            cJSON_AddNumberToObject(s, "rate", step.rate);
        } else {
            cJSON_AddNumberToObject(s, "holdTime", step.value);
        }
        cJSON_AddItemToArray(schedArr, s);
    }
    cJSON_AddItemToObject(root, "steps", schedArr);
    cJSON_AddStringToObject(root, "name", loadedScheduleName.c_str());
    
    char *rendered = cJSON_PrintUnformatted(root);
    const std::string out = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(root);
    
    (void)kiln_fs_write_text_atomic(RECOVERY_FILE, out);
}

void ThermalController::loadRecoveryStateLocked() {
    const std::string file = kiln_fs_read_text(RECOVERY_FILE);
    if (file.empty()) return;

    cJSON *root = cJSON_Parse(file.c_str());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "Found recovery state. Restoring...");

    // 1. Restore schedule
    activeSchedule.clear();
    cJSON *steps = cJSON_GetObjectItem(root, "steps");
    if (cJSON_IsArray(steps)) {
        cJSON *step = NULL;
        cJSON_ArrayForEach(step, steps) {
            if (!cJSON_IsObject(step)) continue;
            ScheduleStep s;
            const cJSON *typeItem = cJSON_GetObjectItem(step, "type");
            std::string type = (cJSON_IsString(typeItem) && typeItem->valuestring) ? typeItem->valuestring : "ramp";
            s.type = (type == "hold") ? 1 : 0;
            s.rate = 9999.0f;
            
            if (s.type == 0) { // RAMP
                const cJSON *targetItem = cJSON_GetObjectItem(step, "target");
                s.value = cJSON_IsNumber(targetItem) ? (float)targetItem->valuedouble : 0.0f;
                const cJSON *rateItem = cJSON_GetObjectItem(step, "rate");
                if (cJSON_IsNumber(rateItem)) s.rate = (float)rateItem->valuedouble;
            } else { // HOLD
                const cJSON *holdTimeItem = cJSON_GetObjectItem(step, "holdTime");
                s.value = cJSON_IsNumber(holdTimeItem) ? (float)holdTimeItem->valuedouble : 0.0f;
            }
            activeSchedule.push_back(s);
        }
    }

    const cJSON *nameItem = cJSON_GetObjectItem(root, "name");
    if (cJSON_IsString(nameItem) && nameItem->valuestring) {
        loadedScheduleName = nameItem->valuestring;
    } else {
        loadedScheduleName = "Recovered Schedule";
    }

    if (activeSchedule.empty()) {
        cJSON_Delete(root);
        return; // Nothing to recover
    }

    // 2. Restore state
    state.totalSteps = activeSchedule.size();
    
    const cJSON *stepItem = cJSON_GetObjectItem(root, "currentStep");
    if (cJSON_IsNumber(stepItem)) state.currentStep = stepItem->valueint;
    
    const cJSON *startTimeItem = cJSON_GetObjectItem(root, "startTime");
    if (cJSON_IsNumber(startTimeItem)) state.startTime = (uint64_t)startTimeItem->valuedouble;
    
    const cJSON *stepStartTimeItem = cJSON_GetObjectItem(root, "stepStartTime");
    if (cJSON_IsNumber(stepStartTimeItem)) stepStartTime = (uint64_t)stepStartTimeItem->valuedouble;
    
    const cJSON *stepStartTempItem = cJSON_GetObjectItem(root, "stepStartTemp");
    if (cJSON_IsNumber(stepStartTempItem)) stepStartTemp = (float)stepStartTempItem->valuedouble;

    cJSON_Delete(root);

    // 3. Restart firing
    state.isFiring = true;
    state.status = KILN_RUNNING;
    state.errorMsg = "Recovered from power loss";
    state.timeRemaining = -1;
    windowStartTime = esp_timer_get_time() / 1000;
    
    heater_hal_set_safety_relay(true);
    historyStartLocked((uint64_t)(esp_timer_get_time() / 1000ULL));
    
    ESP_LOGW(TAG, "Kiln successfully recovered to step %d", state.currentStep);
}

void ThermalController::clearRecoveryStateLocked() {
    (void)unlink(RECOVERY_FILE);
}

bool ThermalController::setTemperatureOffset(float offsetC) {
    if (!mutex) return false;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    temperatureOffsetC = std::max(-100.0f, std::min(100.0f, offsetC));
    persistRuntimeSettingsLocked();
    xSemaphoreGiveRecursive(mutex);
    return true;
}

float ThermalController::getTemperatureOffset() {
    if (!mutex) return 0.0f;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    const float v = temperatureOffsetC;
    xSemaphoreGiveRecursive(mutex);
    return v;
}

bool ThermalController::setUserMaxTemperatureC(float maxC) {
    if (!mutex) return false;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    userMaxTempC = std::max(100.0f, std::min((float)MAX_TEMP_SAFETY, maxC));
    xSemaphoreGiveRecursive(mutex);
    return true;
}

float ThermalController::getUserMaxTemperatureC() {
    if (!mutex) return (float)MAX_TEMP_SAFETY;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    const float v = userMaxTempC;
    xSemaphoreGiveRecursive(mutex);
    return v;
}

float ThermalController::getLoadedScheduleMaxTargetC() {
    if (!mutex) return 0.0f;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    float maxTarget = 0.0f;
    for (const auto &step : activeSchedule) {
        if (step.type == 0) {
            maxTarget = std::max(maxTarget, step.value);
        }
    }
    xSemaphoreGiveRecursive(mutex);
    return maxTarget;
}

bool ThermalController::setAutotuneTargetC(float targetC) {
    if (!mutex) return false;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    autotuneTargetC = std::max(100.0f, std::min(1200.0f, targetC));
    persistRuntimeSettingsLocked();
    xSemaphoreGiveRecursive(mutex);
    return true;
}

float ThermalController::getAutotuneTargetC() {
    if (!mutex) return 600.0f;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    const float v = autotuneTargetC;
    xSemaphoreGiveRecursive(mutex);
    return v;
}

ThermalController::AutoTuneStatus ThermalController::getAutotuneStatus() {
    AutoTuneStatus s{};
    if (!mutex) return s;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    s.active = tune.active;
    s.heaterOn = tune.heaterOn;
    s.setpointC = tune.setpointC;
    s.cycles = tune.periods;
    s.valid_cycles = tune.validCycles;
    s.total_cycles = tune.totalCycles;
    s.ku = tune.ku;
    s.pu_s = tune.pu_s;
    s.period_cv = tune.periodCv;
    s.amp_cv = tune.ampCv;
    s.quality = tune.quality;
    s.confidence = tune.confidence;
    s.kp = runtimeKp;
    s.ki = runtimeKi;
    s.kd = runtimeKd;
    xSemaphoreGiveRecursive(mutex);
    return s;
}

ThermalController::PidTunings ThermalController::getPidTunings() {
    PidTunings p{};
    p.kp_default = Kp;
    p.ki_default = Ki;
    p.kd_default = Kd;

    if (!mutex) return p;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    p.kp = runtimeKp;
    p.ki = runtimeKi;
    p.kd = runtimeKd;
    p.temp_offset_c = temperatureOffsetC;
    xSemaphoreGiveRecursive(mutex);
    return p;
}

bool ThermalController::resetPidToDefaults() {
    if (!mutex) return false;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);

    if (tune.active) {
        xSemaphoreGiveRecursive(mutex);
        return false;
    }

    runtimeKp = Kp;
    runtimeKi = Ki;
    runtimeKd = Kd;
    if (pid) pid->SetTunings(runtimeKp, runtimeKi, runtimeKd);
    persistRuntimeSettingsLocked();
    append_event_log("PID", "reset_defaults");

    xSemaphoreGiveRecursive(mutex);
    return true;
}

bool ThermalController::startAutotune(float setpointC) {
    if (!mutex) return false;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);

    if (kiln_fault_is_active() || state.isFiring || tune.active) {
        xSemaphoreGiveRecursive(mutex);
        return false;
    }
    if (!isSensorHealthy()) {
        xSemaphoreGiveRecursive(mutex);
        return false;
    }

    tune = AutoTune{};
    tune.active = true;
    tune.heaterOn = true;
    const float max_allowed = std::max(50.0f, std::min((float)MAX_TEMP_SAFETY - 25.0f, userMaxTempC - 25.0f));
    tune.setpointC = std::max(50.0f, std::min(max_allowed, setpointC));
    autotuneTargetC = tune.setpointC;
    persistRuntimeSettingsLocked();
    tune.hysteresisC = 5.0f;
    tune.startMs = (uint64_t)(esp_timer_get_time() / 1000ULL);
    tune.lastSwitchMs = tune.startMs;
    tune.lastOnMs = tune.startMs;
    tune.peakMax = state.currentTemp;
    tune.peakMin = state.currentTemp;
    tune.lastHigh = state.currentTemp;
    tune.lastLow = state.currentTemp;

    heater_hal_set_safety_relay(true);
    heater_hal_set_ssr(true);

    state.isFiring = true;
    state.status = KILN_TUNING;
    state.targetTemp = tune.setpointC;
    setpoint = state.targetTemp;
    state.errorMsg = "Autotune running";

    {
        char msg[64];
        snprintf(msg, sizeof(msg), "start setpoint=%.1fC", (double)tune.setpointC);
        append_event_log("AUTOTUNE", msg);
    }

    xSemaphoreGiveRecursive(mutex);
    return true;
}

void ThermalController::stopAutotune(const std::string &reason) {
    if (!mutex) return;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    if (tune.active) {
        tune.active = false;
        heater_hal_all_off();
        state.isFiring = false;
        state.status = KILN_IDLE;
        state.targetTemp = 0.0f;
        state.errorMsg = reason;
        append_event_log("AUTOTUNE", "stop");
    }
    xSemaphoreGiveRecursive(mutex);
}

void ThermalController::autotuneLoopLocked(uint64_t now_ms) {
    static constexpr uint64_t MAX_TUNE_MS = 45ULL * 60ULL * 1000ULL;

    state.isFiring = true;
    state.status = KILN_TUNING;
    state.targetTemp = tune.setpointC;
    setpoint = state.targetTemp;
    state.timeRemaining = -1;

    if (!isSensorHealthy() && sensorInvalidStreak >= SENSOR_INVALID_LATCH_COUNT) {
        append_event_log("AUTOTUNE_FAIL", "sensor fault");
        heater_hal_all_off();
        tune.active = false;
        state.isFiring = false;
        output = 0;
        setpoint = 0;
        state.targetTemp = 0;
        kiln_fault_set(KILN_FAULT_AUTOTUNE_FAILED, "Autotune sensor fault");
        return;
    }
    if (now_ms - tune.startMs > MAX_TUNE_MS) {
        append_event_log("AUTOTUNE_FAIL", "timeout");
        heater_hal_all_off();
        tune.active = false;
        state.isFiring = false;
        output = 0;
        setpoint = 0;
        state.targetTemp = 0;
        kiln_fault_set(KILN_FAULT_AUTOTUNE_FAILED, "Autotune timeout");
        return;
    }

    // Relay control around setpoint
    if (tune.heaterOn) {
        tune.peakMax = std::max(tune.peakMax, state.currentTemp);
        if (state.currentTemp >= tune.setpointC + tune.hysteresisC) {
            tune.heaterOn = false;
            tune.lastHigh = tune.peakMax;
            tune.peakMin = state.currentTemp;
        }
    } else {
        tune.peakMin = std::min(tune.peakMin, state.currentTemp);
        if (state.currentTemp <= tune.setpointC - tune.hysteresisC) {
            tune.heaterOn = true;
            tune.lastLow = tune.peakMin;

            if (tune.lastOnMs != 0) {
                const uint64_t period = now_ms - tune.lastOnMs;
                if (period > 0) {
                    const float amp = (tune.lastHigh - tune.lastLow) * 0.5f;
                    tune.totalCycles++;
                    if (period >= 5000 && amp > 0.1f && tune.periods < 6) {
                        tune.periodMs[tune.periods] = (uint32_t)period;
                        tune.ampC[tune.periods] = amp;
                        tune.periods++;
                    }
                }
            }
            tune.lastOnMs = now_ms;
            tune.peakMax = state.currentTemp;
        }
    }

    heater_hal_set_safety_relay(true);
    heater_hal_set_ssr(tune.heaterOn);
    output = tune.heaterOn ? PID_WINDOW_SIZE : 0;

    char msg[64];
    snprintf(msg, sizeof(msg), "Autotune %d/6", tune.periods);
    state.errorMsg = msg;

    if (tune.periods >= 6) {
        autotuneFinalizeLocked(now_ms);
    }
}

void ThermalController::autotuneFinalizeLocked(uint64_t now_ms) {
    (void)now_ms;
    double pu_ms_sum = 0;
    double amp_sum = 0;
    double pu_sq_sum = 0;
    double amp_sq_sum = 0;
    int n = 0;
    for (int i = 1; i < tune.periods; i++) { // skip first cycle (often noisy)
        if (tune.periodMs[i] > 0 && tune.ampC[i] > 0.1f) {
            const double p = (double)tune.periodMs[i];
            const double a = (double)tune.ampC[i];
            pu_ms_sum += p;
            amp_sum += a;
            pu_sq_sum += p * p;
            amp_sq_sum += a * a;
            n++;
        }
    }
    tune.validCycles = n;
    if (n < 3) {
        append_event_log("AUTOTUNE_FAIL", "insufficient data");
        heater_hal_all_off();
        tune.active = false;
        state.isFiring = false;
        output = 0;
        setpoint = 0;
        state.targetTemp = 0;
        kiln_fault_set(KILN_FAULT_AUTOTUNE_FAILED, "Autotune insufficient data");
        return;
    }

    const double pu_ms = pu_ms_sum / (double)n;
    const double a = amp_sum / (double)n;
    const double pu_var = std::max(0.0, (pu_sq_sum / (double)n) - (pu_ms * pu_ms));
    const double amp_var = std::max(0.0, (amp_sq_sum / (double)n) - (a * a));
    const double pu_std = std::sqrt(pu_var);
    const double amp_std = std::sqrt(amp_var);
    const double period_cv = pu_ms > 1e-6 ? (pu_std / pu_ms) : 1.0;
    const double amp_cv = a > 1e-6 ? (amp_std / a) : 1.0;
    const double quality_01 = std::max(0.0, std::min(1.0, 1.0 - (0.6 * period_cv + 0.4 * amp_cv)));
    const int total_cycles = std::max(1, tune.totalCycles);
    const double valid_ratio = (double)n / (double)total_cycles;
    const double sensor_factor = isSensorHealthy() ? 1.0 : 0.7;
    const double confidence_01 = std::max(0.0, std::min(1.0, 0.5 * valid_ratio + 0.4 * quality_01 + 0.1 * sensor_factor));

    const double d = 50.0; // relay amplitude (0..100% => +/-50%)
    const double pi = 3.14159265358979323846;
    const double ku = (4.0 * d) / (pi * a);
    const double pu_s = pu_ms / 1000.0;

    // Tyreus-Luyben (conservative for thermal systems)
    const double kp_new = ku / 2.2;
    const double ti = 2.2 * pu_s;
    const double td = pu_s / 6.3;
    const double ki_new = kp_new / std::max(1.0, ti);
    const double kd_new = kp_new * td;

    runtimeKp = std::max(0.0, kp_new);
    runtimeKi = std::max(0.0, ki_new);
    runtimeKd = std::max(0.0, kd_new);
    pid->SetTunings(runtimeKp, runtimeKi, runtimeKd);
    persistRuntimeSettingsLocked();

    tune.ku = (float)ku;
    tune.pu_s = (float)pu_s;
    tune.periodCv = (float)period_cv;
    tune.ampCv = (float)amp_cv;
    tune.quality = (float)(quality_01 * 100.0);
    tune.confidence = (float)(confidence_01 * 100.0);

    heater_hal_all_off();
    tune.active = false;
    state.isFiring = false;
    state.status = KILN_IDLE;
    state.targetTemp = 0.0f;

    char done[96];
    snprintf(done, sizeof(done), "Autotune OK kp=%.2f ki=%.3f kd=%.2f Q=%.0f%% C=%.0f%%",
             runtimeKp, runtimeKi, runtimeKd, (double)tune.quality, (double)tune.confidence);
    state.errorMsg = done;
    append_event_log("AUTOTUNE", "done");

    ESP_LOGI(TAG, "Autotune done: Ku=%.3f Pu=%.1fs -> Kp=%.3f Ki=%.4f Kd=%.3f",
             ku, pu_s, runtimeKp, runtimeKi, runtimeKd);
}

bool ThermalController::isSensorHealthy() {
    return lastSensorOk &&
           sensorValidStreak > 0 &&
           std::isfinite(state.currentTemp) &&
           state.currentTemp <= SENSOR_TEMP_MAX_C &&
           state.currentTemp >= SENSOR_TEMP_MIN_C;
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
        const float target = std::min(step.value, userMaxTempC);
        float rate = step.rate;
        if (!std::isfinite(rate) || rate <= 0.0f) rate = 9999.0f;
        const float elapsed_min = (float)elapsed / 60.0f;
        float rampTarget = target;
        if (target >= stepStartTemp) {
            rampTarget = std::min(target, stepStartTemp + rate * elapsed_min);
        } else {
            rampTarget = std::max(target, stepStartTemp - rate * elapsed_min);
        }
        state.status = (state.currentTemp > target + 2.0f) ? KILN_COOLING : KILN_RUNNING;
        state.targetTemp = rampTarget;
        setpoint = state.targetTemp;
        state.timeRemaining = -1;
        
        if (state.currentTemp >= target - 1.0f) { // Hysteresis 1.0C
            state.currentStep++;
            ESP_LOGI(TAG, "RAMP Step %d completed. Moving to next step.", state.currentStep);
            stepStartTime = esp_timer_get_time() / 1000;
            stepStartTemp = state.currentTemp;
        }

    } else if (step.type == 1) { // HOLD
        state.status = KILN_HOLD;
        setpoint = state.targetTemp; // Keep previous target

        // Remaining time: estimate from current + future HOLD steps only.
        const int elapsed_min = (int)(elapsed / 60U);
        const int this_hold_min = (int)std::round(step.value);
        int remaining_min = std::max(0, this_hold_min - elapsed_min);
        for (size_t i = (size_t)state.currentStep + 1; i < activeSchedule.size(); i++) {
            if (activeSchedule[i].type == 1) remaining_min += (int)std::round(activeSchedule[i].value);
        }
        state.timeRemaining = remaining_min;

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
    
    // Simple Gain Scheduling based on temperature
    // These are example bands. In a real kiln, > 600C usually requires more aggressive driving
    // due to higher heat loss, and < 300C requires gentle driving.
    if (state.currentTemp > 600.0f) {
        pid->SetTunings(runtimeKp * 1.5, runtimeKi * 1.2, runtimeKd * 0.8);
    } else if (state.currentTemp < 300.0f) {
        pid->SetTunings(runtimeKp * 0.8, runtimeKi * 0.8, runtimeKd * 1.2);
    } else {
        pid->SetTunings(runtimeKp, runtimeKi, runtimeKd);
    }

    pid->Compute();
}

void ThermalController::driveSSR() {
    uint64_t now = esp_timer_get_time() / 1000;
    if (now - windowStartTime > PID_WINDOW_SIZE) {
        windowStartTime += PID_WINDOW_SIZE;
    }
    
    heater_hal_set_safety_relay(true);
    heater_hal_set_ssr(output > (now - windowStartTime));
}

void ThermalController::historyStartLocked(uint64_t now_ms) {
    historyActive = true;
    historyStartMs = now_ms;
    historyPeakTemp = state.currentTemp;
    lastHistorySampleMs = 0;
    historyPoints.clear();

    char idbuf[24];
    snprintf(idbuf, sizeof(idbuf), "%llu", (unsigned long long)now_ms);
    historyId = idbuf;

    // First point
    historySampleLocked(now_ms);
}

void ThermalController::historySampleLocked(uint64_t now_ms) {
    if (!historyActive) return;

    // Sample every 10 seconds
    if (lastHistorySampleMs != 0 && (now_ms - lastHistorySampleMs) < 10000) return;
    lastHistorySampleMs = now_ms;

    if (!std::isnan(state.currentTemp)) {
        historyPeakTemp = std::max(historyPeakTemp, state.currentTemp);
    }

    // Cap to avoid huge memory/JSON
    if (historyPoints.size() >= 2000) {
        historyPoints.erase(historyPoints.begin(), historyPoints.begin() + 200);
    }

    HistoryPoint p{};
    p.ts_ms = now_ms;
    p.temp = state.currentTemp;
    p.target = state.targetTemp;
    historyPoints.push_back(p);
}

void ThermalController::historyFinalizeLocked(uint64_t now_ms, const char *status) {
    if (!historyActive) return;

    // Build summary
    const uint64_t start_ms = historyStartMs;
    const uint64_t end_ms = now_ms;
    const uint64_t duration_s = (end_ms > start_ms) ? ((end_ms - start_ms) / 1000ULL) : 0ULL;

    const int totalSteps = state.totalSteps;
    const int completedSteps = std::max(0, std::min(state.currentStep, state.totalSteps));

    double err_sq_sum = 0.0;
    double abs_err_sum = 0.0;
    double overshoot_max = 0.0;
    double overshoot_temp_sum = 0.0;
    int overshoot_count = 0;
    double target_max = 0.0;
    for (const HistoryPoint &p : historyPoints) {
        const double target = (double)p.target;
        const double temp = (double)p.temp;
        const double err = temp - target;
        err_sq_sum += err * err;
        abs_err_sum += std::fabs(err);
        target_max = std::max(target_max, target);
        if (target > 1.0 && err > 0.0) {
            overshoot_count++;
            overshoot_temp_sum += err;
            overshoot_max = std::max(overshoot_max, err);
        }
    }
    const double sample_count = historyPoints.empty() ? 0.0 : (double)historyPoints.size();
    const double err_rms = sample_count > 0.0 ? std::sqrt(err_sq_sum / sample_count) : 0.0;
    const double err_mae = sample_count > 0.0 ? (abs_err_sum / sample_count) : 0.0;
    const double overshoot_avg = overshoot_count > 0 ? (overshoot_temp_sum / (double)overshoot_count) : 0.0;
    const double overshoot_pct_max = target_max > 1.0 ? (overshoot_max * 100.0 / target_max) : 0.0;
    const double duration_h = duration_s > 0 ? ((double)duration_s / 3600.0) : 0.0;
    const double energy_index = duration_h * (std::max(1.0, target_max) / 100.0);
    const double q_err = std::max(0.0, std::min(1.0, 1.0 - (err_rms / 35.0)));
    const double q_over = std::max(0.0, std::min(1.0, 1.0 - (overshoot_pct_max / 12.0)));
    const double q_fault = kiln_fault_is_active() ? 0.4 : 1.0;
    const double quality_score = std::max(0.0, std::min(100.0, (0.6 * q_err + 0.3 * q_over + 0.1 * q_fault) * 100.0));

    cJSON *summary = cJSON_CreateObject();
    cJSON_AddStringToObject(summary, "id", historyId.c_str());
    cJSON_AddStringToObject(summary, "scheduleName", loadedScheduleName.c_str());
    cJSON_AddNumberToObject(summary, "startTime", (double)start_ms);
    cJSON_AddNumberToObject(summary, "endTime", (double)end_ms);
    cJSON_AddNumberToObject(summary, "duration", (double)duration_s);
    cJSON_AddStringToObject(summary, "status", status ? status : "STOPPED");
    if (!std::isnan(historyPeakTemp)) cJSON_AddNumberToObject(summary, "peakTemp", (double)historyPeakTemp);
    else cJSON_AddNullToObject(summary, "peakTemp");
    cJSON_AddNumberToObject(summary, "totalSteps", (double)totalSteps);
    cJSON_AddNumberToObject(summary, "completedSteps", (double)completedSteps);
    cJSON *kpi = cJSON_CreateObject();
    cJSON_AddNumberToObject(kpi, "tracking_error_rms", err_rms);
    cJSON_AddNumberToObject(kpi, "tracking_error_mae", err_mae);
    cJSON_AddNumberToObject(kpi, "overshoot_max", overshoot_max);
    cJSON_AddNumberToObject(kpi, "overshoot_avg", overshoot_avg);
    cJSON_AddNumberToObject(kpi, "overshoot_pct_max", overshoot_pct_max);
    cJSON_AddNumberToObject(kpi, "energy_index", energy_index);
    cJSON_AddNumberToObject(kpi, "quality_score", quality_score);
    cJSON_AddNumberToObject(kpi, "samples", sample_count);
    cJSON_AddItemToObject(summary, "kpi", kpi);

    // Update list file
    cJSON *arr = nullptr;
    const std::string list = kiln_fs_read_text("/littlefs/history.json");
    if (!list.empty()) {
        cJSON *parsed = cJSON_Parse(list.c_str());
        if (parsed && cJSON_IsArray(parsed)) arr = parsed;
        else if (parsed) cJSON_Delete(parsed);
    }
    if (!arr) arr = cJSON_CreateArray();

    // Prepend
    cJSON_InsertItemInArray(arr, 0, cJSON_Duplicate(summary, 1));
    while (cJSON_GetArraySize(arr) > 30) {
        cJSON_DeleteItemFromArray(arr, cJSON_GetArraySize(arr) - 1);
    }

    char *renderedArr = cJSON_PrintUnformatted(arr);
    std::string outArr = renderedArr ? renderedArr : "[]";
    if (renderedArr) free(renderedArr);
    cJSON_Delete(arr);

    (void)kiln_fs_write_text_atomic("/littlefs/history.json", outArr);

    // Detail file
    cJSON *detail = cJSON_CreateObject();
    cJSON_AddItemToObject(detail, "summary", summary); // takes ownership
    cJSON *data = cJSON_CreateArray();
    for (const HistoryPoint &p : historyPoints) {
        cJSON *dp = cJSON_CreateObject();
        cJSON_AddNumberToObject(dp, "timestamp", (double)p.ts_ms);
        cJSON_AddNumberToObject(dp, "temp", (double)p.temp);
        cJSON_AddNumberToObject(dp, "target", (double)p.target);
        cJSON_AddItemToArray(data, dp);
    }
    cJSON_AddItemToObject(detail, "data", data);

    char *renderedDetail = cJSON_PrintUnformatted(detail);
    std::string outDetail = renderedDetail ? renderedDetail : "{}";
    if (renderedDetail) free(renderedDetail);
    cJSON_Delete(detail);

    std::string path = "/littlefs/history_";
    path += historyId;
    path += ".json";
    (void)kiln_fs_write_text_atomic(path.c_str(), outDetail);

    historyActive = false;
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

    // Best-effort schedule name for UI/history
    loadedScheduleName.clear();
    const cJSON *nameItem = cJSON_GetObjectItem(root, "name");
    if (!cJSON_IsString(nameItem) || !nameItem->valuestring || !nameItem->valuestring[0]) {
        nameItem = cJSON_GetObjectItem(root, "title");
    }
    if (!cJSON_IsString(nameItem) || !nameItem->valuestring || !nameItem->valuestring[0]) {
        nameItem = cJSON_GetObjectItem(root, "id");
    }
    if (cJSON_IsString(nameItem) && nameItem->valuestring) loadedScheduleName = nameItem->valuestring;
    if (loadedScheduleName.empty()) loadedScheduleName = "Unnamed";
    
    // Clear old schedule
    activeSchedule.clear();
    
    // Populate new schedule
    cJSON *steps = cJSON_GetObjectItem(root, "steps");
    if (!cJSON_IsArray(steps)) steps = cJSON_GetObjectItem(root, "segments");
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
        s.rate = 9999.0f;
        
        if (s.type == 0) { // RAMP
            const cJSON *targetItem = cJSON_GetObjectItem(step, "target");
            s.value = cJSON_IsNumber(targetItem) ? (float)targetItem->valuedouble : 0.0f;
            const cJSON *rateItem = cJSON_GetObjectItem(step, "rate");
            if (cJSON_IsNumber(rateItem)) s.rate = (float)rateItem->valuedouble;
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
    state.timeRemaining = -1;
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

    if (kiln_fault_is_active()) {
        state.errorMsg = "Fault active";
        xSemaphoreGiveRecursive(mutex);
        return;
    }
    
    // Reset State
    state.isFiring = true;
    state.status = KILN_RUNNING;
    state.currentStep = 0;
    state.errorMsg = "";
    state.startTime = (uint64_t)(esp_timer_get_time() / 1000000ULL);
    state.duration = 0;
    state.timeRemaining = -1;
    stepStartTime = esp_timer_get_time() / 1000;
    stepStartTemp = state.currentTemp;
    windowStartTime = esp_timer_get_time() / 1000;
    
    // Enable Safety Relay (SSR control happens in driveSSR)
    heater_hal_set_safety_relay(true);
    
    ESP_LOGI(TAG, "Schedule Started");

    historyStartLocked((uint64_t)(esp_timer_get_time() / 1000ULL));
    
    xSemaphoreGiveRecursive(mutex);
}

void ThermalController::stop(const std::string& reason) {
    if (!mutex) return;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);

    tune.active = false;
    
    const bool wasFiring = state.isFiring;
    state.isFiring = false;
    state.status = KILN_IDLE;
    state.errorMsg = reason;
    state.duration = 0;
    state.timeRemaining = -1;
    
    heater_hal_all_off();
    output = 0;
    setpoint = 0;
    state.targetTemp = 0;
    
    clearRecoveryStateLocked();

    ESP_LOGI(TAG, "KILN STOPPED: %s", reason.c_str());

    if (wasFiring && historyActive) {
        const char *st = "STOPPED";
        if (state.status == KILN_COMPLETE || reason == "Schedule Complete") st = "COMPLETED";
        historyFinalizeLocked((uint64_t)(esp_timer_get_time() / 1000ULL), st);
    }
    
    xSemaphoreGiveRecursive(mutex);
}

void ThermalController::emergencyStop(const std::string& reason) {
    if (!mutex) return;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);

    tune.active = false;

    // Explicitly call stop logic inline to avoid recursion deadlock if stop() also takes mutex
    // But since it's recursive mutex, it should be fine.
    // However, let's just duplicate the shutdown logic to be absolutely safe and fast.

    state.isFiring = false;
    state.status = KILN_FAULT;
    state.errorMsg = reason;
    state.duration = 0;
    state.timeRemaining = -1;

    kiln_fault_set(KILN_FAULT_EMERGENCY_STOP, reason.c_str());
    heater_hal_all_off();
    output = 0;
    setpoint = 0;
    state.targetTemp = 0;

    clearRecoveryStateLocked();

    ESP_LOGE(TAG, "EMERGENCY STOP: %s", reason.c_str());

    if (historyActive) historyFinalizeLocked((uint64_t)(esp_timer_get_time() / 1000ULL), "ERROR");

    xSemaphoreGiveRecursive(mutex);
}

bool ThermalController::clearFault() {
    if (!mutex) return false;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);

    tune.active = false;

    heater_hal_all_off();

    // Safety policy: refuse clearing when too hot.
    if (state.currentTemp > FAULT_CLEAR_MAX_TEMP_C) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Too hot to reset fault (%.1fC)", (double)state.currentTemp);
        state.errorMsg = msg;
        kiln_fault_update_reason(msg);
        xSemaphoreGiveRecursive(mutex);
        return false;
    }

    const kiln_fault_t f = kiln_fault_get();
    const bool sensor_fault =
        (f.code == KILN_FAULT_SENSOR_NAN || f.code == KILN_FAULT_SENSOR_OOR ||
         f.code == KILN_FAULT_SENSOR_OPEN || f.code == KILN_FAULT_SENSOR_SHORT ||
         f.code == KILN_FAULT_SENSOR_STUCK);
    if (sensor_fault && !sensorRecoveredForClearLocked((uint64_t)(esp_timer_get_time() / 1000ULL))) {
        kiln_fault_update_reason("Sensor not stable yet, wait for valid readings");
        state.errorMsg = "Sensor not stable yet";
        xSemaphoreGiveRecursive(mutex);
        return false;
    }

    kiln_fault_clear();

    state.isFiring = false;
    state.status = KILN_IDLE;
    state.errorMsg = "";
    state.duration = 0;
    state.timeRemaining = -1;

    output = 0;
    setpoint = 0;
    state.targetTemp = 0;

    xSemaphoreGiveRecursive(mutex);
    return true;
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

void ThermalController::setCurrentRampRate(float rateCPerMin) {
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    if (state.isFiring && state.currentStep < activeSchedule.size()) {
        if (activeSchedule[state.currentStep].type == 0 && std::isfinite(rateCPerMin) && rateCPerMin > 0.0f) {
            activeSchedule[state.currentStep].rate = rateCPerMin;
            ESP_LOGI(TAG, "Updated current RAMP rate to %.2f C/min", (double)rateCPerMin);
        } else {
            ESP_LOGW(TAG, "Attempted to set ramp rate but current step is not RAMP or payload invalid.");
        }
    } else {
        ESP_LOGW(TAG, "Attempted to set ramp rate but kiln is not firing.");
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

ThermoStats ThermalController::getThermoStats() {
    if (mutex) xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    ThermoStats s{};
    s.raw = thermoLastRaw;
    s.readCount = thermoReadCount;
    if (mutex) xSemaphoreGiveRecursive(mutex);
    return s;
}

SafetyStats ThermalController::getSafetyStats() {
    const kiln_fault_t f = kiln_fault_get();
    SafetyStats s{};
    s.faultActive = f.active;
    s.faultCode = (uint32_t)f.code;
    s.faultSinceMs = f.since_ms;
    memcpy(s.faultReason, f.reason, sizeof(s.faultReason) - 1);
    s.faultReason[sizeof(s.faultReason) - 1] = 0;
    return s;
}

ThermalController thermalCtrl;
