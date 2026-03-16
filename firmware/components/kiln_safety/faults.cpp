#include "kiln_safety/faults.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static kiln_fault_t s_fault = {false, KILN_FAULT_NONE, 0, {0}};

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000ULL); }

void kiln_fault_set(kiln_fault_code_t code, const char *reason) {
    if (code == KILN_FAULT_NONE) return;
    if (!reason) reason = "";

    taskENTER_CRITICAL(&s_mux);
    if (!s_fault.active) {
        s_fault.active = true;
        s_fault.code = code;
        s_fault.since_ms = now_ms();
        strncpy(s_fault.reason, reason, sizeof(s_fault.reason) - 1);
        s_fault.reason[sizeof(s_fault.reason) - 1] = 0;
    }
    taskEXIT_CRITICAL(&s_mux);
}

void kiln_fault_update_reason(const char *reason) {
    if (!reason) reason = "";
    taskENTER_CRITICAL(&s_mux);
    if (s_fault.active) {
        strncpy(s_fault.reason, reason, sizeof(s_fault.reason) - 1);
        s_fault.reason[sizeof(s_fault.reason) - 1] = 0;
    }
    taskEXIT_CRITICAL(&s_mux);
}

void kiln_fault_clear(void) {
    taskENTER_CRITICAL(&s_mux);
    s_fault.active = false;
    s_fault.code = KILN_FAULT_NONE;
    s_fault.since_ms = 0;
    s_fault.reason[0] = 0;
    taskEXIT_CRITICAL(&s_mux);
}

kiln_fault_t kiln_fault_get(void) {
    taskENTER_CRITICAL(&s_mux);
    kiln_fault_t copy = s_fault;
    taskEXIT_CRITICAL(&s_mux);
    return copy;
}

bool kiln_fault_is_active(void) {
    taskENTER_CRITICAL(&s_mux);
    const bool active = s_fault.active;
    taskEXIT_CRITICAL(&s_mux);
    return active;
}
