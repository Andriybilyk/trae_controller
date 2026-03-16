#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float current_temp;
    float target_temp;
    int32_t status;
    bool is_firing;
    int32_t time_remaining;
    int32_t current_step;
    int32_t total_steps;
    char error_msg[96];
    bool fault_active;
    char fault_reason[96];
} slint_kiln_state_t;

void slint_bridge_display_init(void);
void slint_bridge_get_state(slint_kiln_state_t *out);
bool slint_bridge_start_schedule_json(const char *json);
bool slint_bridge_load_schedule_json(const char *json);
void slint_bridge_stop(void);

#ifdef __cplusplus
}
#endif
