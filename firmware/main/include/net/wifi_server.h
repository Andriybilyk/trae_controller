#ifndef WIFI_SERVER_H
#define WIFI_SERVER_H

#include "esp_http_server.h"
#include <string>
#include "kiln_control/thermal_control.h"

class WiFiServerManager {
public:
    WiFiServerManager();
    void begin();
    void loop();

    // Broadcast state to all connected clients
    void broadcastState();

    // Schedules sync helpers
    void notifySchedulesChanged(const char *action, const char *name = nullptr);
    uint32_t getSchedulesRevision() const { return schedulesRevision; }

    // Settings/autotune sync helpers
    void notifySettingsChanged(const char *action);
    void notifyAutotuneState(const char *action);
    uint32_t getSettingsRevision() const { return settingsRevision; }
    
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
    static esp_err_t api_touch_pins_get_handler(httpd_req_t *req);
    static esp_err_t api_touch_pins_set_handler(httpd_req_t *req);
    static esp_err_t api_touch_probe_handler(httpd_req_t *req);
    static esp_err_t api_touch_raw_handler(httpd_req_t *req);
    static esp_err_t api_touch_stats_handler(httpd_req_t *req);
    static esp_err_t api_fault_clear_handler(httpd_req_t *req);
    static esp_err_t api_fault_get_handler(httpd_req_t *req);
    static esp_err_t api_history_list_handler(httpd_req_t *req);
    static esp_err_t api_history_detail_handler(httpd_req_t *req);
    static esp_err_t manifest_handler(httpd_req_t *req);
    
    uint64_t lastBroadcast;
    uint32_t schedulesRevision = 0;
    uint32_t settingsRevision = 0;
    bool lastTuneActive = false;
    int lastTuneCycles = -1;
};

extern WiFiServerManager wifiServer;

#endif
