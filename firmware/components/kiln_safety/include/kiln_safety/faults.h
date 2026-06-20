#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KILN_FAULT_NONE = 0,
    KILN_FAULT_SENSOR_NAN = 1,
    KILN_FAULT_SENSOR_OOR = 2,
    KILN_FAULT_OVERTEMP = 3,
    KILN_FAULT_RISE_RATE = 4,
    KILN_FAULT_EMERGENCY_STOP = 5,
    KILN_FAULT_AUTOTUNE_FAILED = 6,
    KILN_FAULT_SENSOR_OPEN = 7,
    KILN_FAULT_SENSOR_SHORT = 8,
    KILN_FAULT_SENSOR_STUCK = 9,
    KILN_FAULT_WATCHDOG = 10,
    KILN_FAULT_SSR = 11,
    KILN_FAULT_SENSOR_SHORT_GND = 12,
    KILN_FAULT_SENSOR_SHORT_VCC = 13,
} kiln_fault_code_t;

typedef struct {
    bool active;
    kiln_fault_code_t code;
    uint64_t since_ms;
    char reason[96];
} kiln_fault_t;

void kiln_fault_set(kiln_fault_code_t code, const char *reason);
void kiln_fault_update_reason(const char *reason);
void kiln_fault_clear(void);
kiln_fault_t kiln_fault_get(void);
bool kiln_fault_is_active(void);

#ifdef __cplusplus
}
#endif
