#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void buzzer_driver_init(void);
void buzzer_driver_tick(void);
void buzzer_driver_beep_segment(void);
void buzzer_driver_beep_complete(void);
bool buzzer_driver_is_busy(void);

#ifdef __cplusplus
}
#endif
