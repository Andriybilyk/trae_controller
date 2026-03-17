#pragma once

#include <stdbool.h>
#include <stdint.h>

void fan_driver_init(void);
void fan_driver_set_manual(bool enabled);
void fan_driver_set_power_percent(uint8_t percent);
void fan_driver_set_auto_enabled(bool enabled);
bool fan_driver_get_auto_enabled(void);
void fan_driver_set_auto_curve(float temp_min_c, float temp_max_c, uint8_t power_min_percent, uint8_t power_max_percent);
void fan_driver_get_auto_curve(float *temp_min_c, float *temp_max_c, uint8_t *power_min_percent, uint8_t *power_max_percent);
void fan_driver_update_from_temperature(float temp_c, bool is_firing);
uint8_t fan_driver_get_effective_power_percent(void);
float fan_driver_get_source_temp_c(void);

bool fan_driver_get_manual(void);
uint8_t fan_driver_get_power_percent(void);
