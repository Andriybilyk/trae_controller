#ifndef WIFI_SERVER_H
#define WIFI_SERVER_H

#include "esp_http_server.h"
#include <string>
#include "thermal_control.h"

class WiFiServerManager {
public:
    WiFiServerManager();
    void begin();
    void loop();

    // Broadcast state to all connected clients
    void broadcastState();
    
    bool isAPMode; 

private:
    httpd_handle_t server;
    
    void setupRoutes();
    static esp_err_t ws_handler(httpd_req_t *req);
    static esp_err_t index_handler(httpd_req_t *req);
    static esp_err_t scan_wifi_handler(httpd_req_t *req);
    static esp_err_t save_wifi_handler(httpd_req_t *req);
    static esp_err_t api_status_handler(httpd_req_t *req);
    static esp_err_t api_schedules_handler(httpd_req_t *req);
    static esp_err_t api_schedules_save_handler(httpd_req_t *req);
    static esp_err_t api_start_handler(httpd_req_t *req);
    static esp_err_t api_stop_handler(httpd_req_t *req);
    static esp_err_t api_skip_handler(httpd_req_t *req);
    static esp_err_t api_add_temp_handler(httpd_req_t *req);
    static esp_err_t api_add_time_handler(httpd_req_t *req);
    static esp_err_t manifest_handler(httpd_req_t *req);
    
    uint64_t lastBroadcast;
};

extern WiFiServerManager wifiServer;

#endif
