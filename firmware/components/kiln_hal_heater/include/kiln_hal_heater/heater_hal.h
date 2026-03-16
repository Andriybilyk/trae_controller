#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void heater_hal_init(void);
void heater_hal_set_ssr(bool on);
void heater_hal_set_safety_relay(bool on);
void heater_hal_all_off(void);

#ifdef __cplusplus
}
#endif

