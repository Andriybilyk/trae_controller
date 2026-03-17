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

void slint_bridge_set_fan_manual(bool enabled);
void slint_bridge_set_fan_power(int32_t percent);
bool slint_bridge_wifi_is_connected(void);
bool slint_bridge_wifi_connect(const char *ssid, const char *password);
void slint_bridge_wifi_disconnect(void);
void slint_bridge_wifi_scan_start(void);
bool slint_bridge_wifi_scan_ready(void);
bool slint_bridge_wifi_scan_copy_results(char *out, int32_t out_len);
uint32_t slint_bridge_get_schedules_revision(void);
void slint_bridge_notify_schedules_changed(void);
void slint_bridge_ui_heartbeat(void);
uint64_t slint_bridge_get_ui_heartbeat_ms(void);

#ifdef __cplusplus
}
#endif
