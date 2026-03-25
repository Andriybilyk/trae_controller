#pragma once

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

bool rtc_ds3231_init(void);
bool rtc_ds3231_read_time(struct tm *out_tm, bool *clock_valid);
bool rtc_ds3231_set_time(const struct tm *tm);
bool rtc_ds3231_apply_time_to_system(void);
bool rtc_ds3231_sync_from_system_time(void);
bool rtc_ds3231_is_clock_valid(bool *clock_valid);

#ifdef __cplusplus
}
#endif
