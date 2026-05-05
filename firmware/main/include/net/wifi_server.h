#ifndef WIFI_SERVER_H
#define WIFI_SERVER_H

#include "esp_http_server.h"
#include <string>
#include <mutex>
#include <atomic>
#include "kiln_control/thermal_control.h"
#include "app/device_commands.h"

struct command_result_snapshot_t {
    bool valid = false;
    bool ok = false;
    uint32_t rev = 0;
    uint64_t ts_ms = 0;
    char action[24]{};
    char source[16]{};
    char code[32]{};
    char message[96]{};
};

class WiFiServerManager {
public:
    WiFiServerManager();
    void begin();
    void loop();

    // Broadcast state to all connected clients
    void broadcastState();

    // Schedules sync helpers
    void notifySchedulesChanged(const char *action, const char *name = nullptr);
    uint32_t getSchedulesRevision() const { return schedulesRevision.load(std::memory_order_relaxed); }

    // Settings/autotune sync helpers
    void notifySettingsChanged(const char *action);
    void notifyAutotuneState(const char *action);
    void notifyCommandResult(const char *action,
                             const device_commands::CommandResult &result,
                             const char *ok_message,
                             const char *source);
    uint32_t getSettingsRevision() const { return settingsRevision.load(std::memory_order_relaxed); }
    uint32_t getCommandResultRevision() const { return commandResultRevision.load(std::memory_order_relaxed); }
    bool copyLastCommandResult(command_result_snapshot_t *out) const;
    
    bool isAPMode; 

private:
    httpd_handle_t server;
    
    void setupRoutes();
    static esp_err_t ws_handler(httpd_req_t *req);
    static esp_err_t index_handler(httpd_req_t *req);
    static esp_err_t scan_wifi_handler(httpd_req_t *req);
    static esp_err_t scan_results_handler(httpd_req_t *req);
    static esp_err_t save_wifi_handler(httpd_req_t *req);
    static esp_err_t api_status_handler(httpd_req_t *req);
    static esp_err_t api_schedules_handler(httpd_req_t *req);
    static esp_err_t api_schedules_save_handler(httpd_req_t *req);
    static esp_err_t api_schedules_delete_handler(httpd_req_t *req);
    static esp_err_t api_start_handler(httpd_req_t *req);
    static esp_err_t api_stop_handler(httpd_req_t *req);
    static esp_err_t api_skip_handler(httpd_req_t *req);
    static esp_err_t api_add_temp_handler(httpd_req_t *req);
    static esp_err_t api_add_time_handler(httpd_req_t *req);
    static esp_err_t api_set_rate_handler(httpd_req_t *req);
    static esp_err_t api_fan_get_handler(httpd_req_t *req);
    static esp_err_t api_fan_set_handler(httpd_req_t *req);
    static esp_err_t api_ota_update_handler(httpd_req_t *req);
    static esp_err_t api_display_get_handler(httpd_req_t *req);
    static esp_err_t api_display_set_handler(httpd_req_t *req);
    static esp_err_t api_settings_get_handler(httpd_req_t *req);
    static esp_err_t api_settings_set_handler(httpd_req_t *req);
    static esp_err_t api_pid_get_handler(httpd_req_t *req);
    static esp_err_t api_pid_reset_handler(httpd_req_t *req);
    static esp_err_t api_autotune_start_handler(httpd_req_t *req);
    static esp_err_t api_autotune_stop_handler(httpd_req_t *req);
    static esp_err_t api_touch_get_handler(httpd_req_t *req);
    static esp_err_t api_touch_spi_get_handler(httpd_req_t *req);
    static esp_err_t api_touch_spi_set_handler(httpd_req_t *req);
    static esp_err_t api_touch_transform_get_handler(httpd_req_t *req);
    static esp_err_t api_touch_transform_set_handler(httpd_req_t *req);
    static esp_err_t api_touch_calibration_get_handler(httpd_req_t *req);
    static esp_err_t api_touch_calibration_set_handler(httpd_req_t *req);
    static esp_err_t api_touch_affine_get_handler(httpd_req_t *req);
    static esp_err_t api_touch_affine_set_handler(httpd_req_t *req);
    static esp_err_t api_touch_grid_get_handler(httpd_req_t *req);
    static esp_err_t api_touch_grid_set_handler(httpd_req_t *req);
    static esp_err_t api_touch_profile_get_handler(httpd_req_t *req);
    static esp_err_t api_touch_profile_set_handler(httpd_req_t *req);
    static esp_err_t api_touch_pins_get_handler(httpd_req_t *req);
    static esp_err_t api_touch_pins_set_handler(httpd_req_t *req);
    static esp_err_t api_touch_probe_handler(httpd_req_t *req);
    static esp_err_t api_touch_raw_handler(httpd_req_t *req);
    static esp_err_t api_touch_stats_handler(httpd_req_t *req);
    static esp_err_t api_fault_clear_handler(httpd_req_t *req);
    static esp_err_t api_fault_get_handler(httpd_req_t *req);
    static esp_err_t api_events_handler(httpd_req_t *req);
    static esp_err_t api_history_list_handler(httpd_req_t *req);
    static esp_err_t api_history_clear_handler(httpd_req_t *req);
    static esp_err_t api_history_detail_handler(httpd_req_t *req);
    static esp_err_t api_backup_export_handler(httpd_req_t *req);
    static esp_err_t api_backup_import_handler(httpd_req_t *req);
    static esp_err_t api_diagnostics_bundle_handler(httpd_req_t *req);
    static esp_err_t api_remote_get_handler(httpd_req_t *req);
    static esp_err_t api_remote_set_handler(httpd_req_t *req);
    static esp_err_t manifest_handler(httpd_req_t *req);
    
    uint64_t lastBroadcast;
    std::atomic<uint32_t> schedulesRevision{0};
    std::atomic<uint32_t> settingsRevision{0};
    std::atomic<uint32_t> commandResultRevision{0};
    mutable std::mutex commandResultMutex;
    command_result_snapshot_t lastCommandResult{};
    std::atomic<bool> lastTuneActive{false};
    std::atomic<int> lastTuneCycles{-1};
    uint64_t lastStartCommandMs = 0;
    uint64_t lastStopCommandMs = 0;
    uint64_t lastSkipCommandMs = 0;
    uint64_t lastAddTempCommandMs = 0;
    uint64_t lastAddTimeCommandMs = 0;
    uint64_t lastSetRateCommandMs = 0;
};

extern WiFiServerManager wifiServer;

#endif
