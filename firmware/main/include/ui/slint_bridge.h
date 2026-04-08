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
    float pzem_voltage;
    float pzem_current;
    float pzem_power;
    bool pzem_ok;
} slint_kiln_state_t;

typedef struct {
    uint32_t rev;
    uint64_t ts_ms;
    bool ok;
    char action[24];
    char source[16];
    char code[32];
    char message[96];
} slint_command_result_t;

void slint_bridge_display_init(void);
void slint_bridge_get_state(slint_kiln_state_t *out);
bool slint_bridge_start_schedule_json(const char *json);
bool slint_bridge_load_schedule_json(const char *json);
void slint_bridge_stop(void);
void slint_bridge_skip_step(void);
void slint_bridge_add_temp(float delta_c);
void slint_bridge_add_time(float delta_minutes);
bool slint_bridge_set_rate(float rate_c_per_min);
bool slint_bridge_start_autotune(float target_c);
void slint_bridge_stop_autotune(void);
float slint_bridge_get_user_max_temp_c(void);
bool slint_bridge_set_user_max_temp_c(float max_c);
float slint_bridge_get_temperature_offset_c(void);
bool slint_bridge_set_temperature_offset_c(float offset_c);
int32_t slint_bridge_get_kiln_wattage(void);
bool slint_bridge_set_kiln_wattage(int32_t watts);
float slint_bridge_get_autotune_target_c(void);
bool slint_bridge_set_autotune_target_c(float target_c);
bool slint_bridge_clear_fault(void);

void slint_bridge_set_fan_manual(bool enabled);
void slint_bridge_set_fan_power(int32_t percent);
bool slint_bridge_wifi_is_connected(void);
bool slint_bridge_wifi_connect(const char *ssid, const char *password);
void slint_bridge_wifi_disconnect(void);
void slint_bridge_wifi_scan_start(void);
bool slint_bridge_wifi_scan_ready(void);
bool slint_bridge_wifi_scan_copy_results(char *out, int32_t out_len);
bool slint_bridge_wifi_server_url_copy(char *out, int32_t out_len);
uint32_t slint_bridge_get_schedules_revision(void);
uint32_t slint_bridge_get_command_result_revision(void);
bool slint_bridge_copy_command_result(slint_command_result_t *out);
void slint_bridge_notify_schedules_changed(void);
void slint_bridge_ui_heartbeat(void);
uint64_t slint_bridge_get_ui_heartbeat_ms(void);
bool slint_bridge_get_time_str(char *out, int32_t out_len);
bool slint_bridge_get_date_str(char *out, int32_t out_len);
bool slint_bridge_get_firmware_version_copy(char *out, int32_t out_len);
bool slint_bridge_get_device_id_copy(char *out, int32_t out_len);
uint64_t slint_bridge_get_uptime_seconds(void);
uint32_t slint_bridge_get_free_heap_bytes(void);
bool slint_bridge_set_time_hm(uint8_t hour, uint8_t minute);
bool slint_bridge_set_date_ymd(uint16_t year, uint8_t month, uint8_t day);
bool slint_bridge_get_language_is_ua(void);
void slint_bridge_set_language_is_ua(bool is_ua);
uint8_t slint_bridge_get_temp_unit(void);
void slint_bridge_set_temp_unit(uint8_t unit);
uint8_t slint_bridge_get_time_format(void);
void slint_bridge_set_time_format(uint8_t fmt);
uint8_t slint_bridge_get_date_format(void);
void slint_bridge_set_date_format(uint8_t fmt);
uint8_t slint_bridge_get_display_brightness(void);
void slint_bridge_set_display_brightness(uint8_t percent);

#ifdef __cplusplus
}
#endif
