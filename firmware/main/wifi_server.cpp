#include "wifi_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "wifi_connection.h"
#include "dns_server.h"
#include <ArduinoJson.h>
#include <string>
#include <cctype>
#include <algorithm>
#include <cstring> // For strlen, strstr
#include <cstdio>  // For FILE, fopen, snprintf

static const char *TAG = "SERVER";

WiFiServerManager::WiFiServerManager() {
    server = NULL;
    lastBroadcast = 0;
    isAPMode = false;
}

static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a')
                a -= 'a'-'A';
            if (a >= 'A')
                a -= ('A' - 10);
            else
                a -= '0';
            if (b >= 'a')
                b -= 'a'-'A';
            if (b >= 'A')
                b -= ('A' - 10);
            else
                b -= '0';
            *dst++ = 16*a+b;
            src+=3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst++ = '\0';
}

esp_err_t WiFiServerManager::index_handler(httpd_req_t *req) {
    // If request is for an API or Asset that wasn't caught by specific handlers,
    // return 404 instead of index.html. This fixes "MIME type text/html" errors for JS/CSS.
    if (strncmp(req->uri, "/api/", 5) == 0 || strncmp(req->uri, "/assets/", 8) == 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    // If Host header is present and is not our IP, redirect to our IP (Captive Portal check)
    // Common checks: connectivitycheck.gstatic.com, captive.apple.com
    char host_str[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host_str, sizeof(host_str)) == ESP_OK) {
        if (wifiServer.isAPMode && strstr(host_str, "192.168.4.1") == NULL) {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
    }

    const char* path = "/littlefs/index.html";
    if (wifiServer.isAPMode) {
        path = "/littlefs/wifi_setup.html";
    }

    FILE* f = fopen(path, "r");
    if (f == NULL) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    // Set correct MIME type for index.html
    httpd_resp_set_type(req, "text/html");

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        httpd_resp_sendstr_chunk(req, line);
    }
    fclose(f);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// Handler for static assets (JS, CSS, etc.)
esp_err_t static_asset_handler(httpd_req_t *req) {
    char filepath[600];
    // Convert URI to file path
    // e.g., /assets/index-da16b362.js -> /littlefs/assets/index-da16b362.js
    if (strlen(req->uri) + 10 > sizeof(filepath)) {
        ESP_LOGE(TAG, "URI too long");
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    snprintf(filepath, sizeof(filepath), "/littlefs%s", req->uri);

    FILE* f = fopen(filepath, "r");
    if (f == NULL) {
        // Try gzipped version
        char gz_filepath[604];
        snprintf(gz_filepath, sizeof(gz_filepath), "%s.gz", filepath);
        f = fopen(gz_filepath, "r");
        if (f) {
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        }
    }

    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open asset: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    // Set content type based on extension
    if (strstr(req->uri, ".js")) {
        httpd_resp_set_type(req, "application/javascript");
    } else if (strstr(req->uri, ".css")) {
        httpd_resp_set_type(req, "text/css");
    } else if (strstr(req->uri, ".ico")) {
        httpd_resp_set_type(req, "image/x-icon");
    } else if (strstr(req->uri, ".svg")) {
        httpd_resp_set_type(req, "image/svg+xml");
    }

    char chunk[512];
    size_t chunksize;
    while ((chunksize = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// Redirect all unknown requests to index (Captive Portal)
esp_err_t redirect_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t vite_handler(httpd_req_t *req) {
    // Return 200 OK to satisfy the browser but do nothing
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t WiFiServerManager::scan_wifi_handler(httpd_req_t *req) {
    std::string json = wifi_scan_networks();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::save_wifi_handler(httpd_req_t *req) {
    char buf[200];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};
    
    // Parse
    char *ssid_ptr = strstr(buf, "ssid=");
    if (ssid_ptr) {
        ssid_ptr += 5;
        char *amp = strchr(ssid_ptr, '&');
        if (amp) {
            *amp = '\0';
            url_decode(ssid, ssid_ptr);
            
            char *pass_ptr = strstr(amp + 1, "password=");
            if (pass_ptr) {
                pass_ptr += 9;
                url_decode(password, pass_ptr);
            }
        } else {
             url_decode(ssid, ssid_ptr);
        }
    }

    ESP_LOGI(TAG, "Saving WiFi: SSID='%s', PASS='%s'", ssid, password);
    wifi_save_creds(ssid, password);

    httpd_resp_sendstr(req, "WiFi Saved. Rebooting...");
    
    // Reboot after delay
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

esp_err_t WiFiServerManager::ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }

    if (ws_pkt.len) {
        buf = (uint8_t*)calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
        
        // Handle message
        std::string msg((char*)buf);
        ESP_LOGI(TAG, "Got packet with message: %s", msg.c_str());
        
        // Try to parse as JSON to detect schedule
        DynamicJsonDocument doc(2048);
        DeserializationError error = deserializeJson(doc, msg);
        
        if (!error) {
            if (doc.containsKey("steps")) {
                // It's a schedule - just load it
                thermalCtrl.loadSchedule(msg);
            } else if ((doc.containsKey("command") && doc["command"] == "stop") || 
                       (doc.containsKey("action") && doc["action"] == "stop")) {
                thermalCtrl.stop("Web Request");
            }
        } 
        
        // Also handle plain text "stop"
        if (msg == "stop" || msg == "STOP") {
            thermalCtrl.stop("Web Request");
        }
    }

    free(buf);
    return ESP_OK;
}

void WiFiServerManager::begin() {
    // 1. Try to connect to saved WiFi
    if (wifi_is_configured()) {
        ESP_LOGI(TAG, "Found saved WiFi credentials. Connecting...");
        if (wifi_init_sta()) {
            ESP_LOGI(TAG, "Connected to WiFi!");
            isAPMode = false;
        } else {
            ESP_LOGE(TAG, "Failed to connect. Starting SoftAP for configuration.");
            wifi_init_softap();
            start_dns_server();
            isAPMode = true;
        }
    } else {
        ESP_LOGI(TAG, "No WiFi configured. Starting SoftAP.");
        wifi_init_softap();
        start_dns_server();
        isAPMode = true;
    }

    // Force stop any firing on boot/reboot just in case
    thermalCtrl.stop("System Boot");

    // 2. Start HTTP Server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192; // Increase stack size to prevent overflow
    config.lru_purge_enable = true;
    config.max_uri_handlers = 20; // Increase handlers limit
    config.uri_match_fn = httpd_uri_match_wildcard; // Enable wildcard matching

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        setupRoutes();
    }
}

void WiFiServerManager::setupRoutes() {
    esp_err_t err;
    
    // API Handlers (Specific)
    httpd_uri_t save_wifi_uri = {
        .uri       = "/save_wifi",
        .method    = HTTP_POST,
        .handler   = save_wifi_handler,
        .user_ctx  = NULL,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    err = httpd_register_uri_handler(server, &save_wifi_uri);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to register save_wifi handler: %d", err);

    httpd_uri_t scan_wifi_uri = {
        .uri       = "/scan_wifi",
        .method    = HTTP_GET,
        .handler   = scan_wifi_handler,
        .user_ctx  = NULL,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    err = httpd_register_uri_handler(server, &scan_wifi_uri);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to register scan_wifi handler: %d", err);

    httpd_uri_t ws_uri = {
        .uri        = "/ws",
        .method     = HTTP_GET,
        .handler    = ws_handler,
        .user_ctx   = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    err = httpd_register_uri_handler(server, &ws_uri);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to register ws handler: %d", err);

    // API Handlers
    httpd_uri_t api_status_uri = {
        .uri       = "/api/status",
        .method    = HTTP_GET,
        .handler   = api_status_handler,
        .user_ctx  = NULL,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    err = httpd_register_uri_handler(server, &api_status_uri);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to register api_status handler: %d", err);

    httpd_uri_t api_schedules_uri = {
        .uri       = "/api/schedules",
        .method    = HTTP_GET,
        .handler   = api_schedules_handler,
        .user_ctx  = NULL,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    err = httpd_register_uri_handler(server, &api_schedules_uri);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to register api_schedules handler: %d", err);

    httpd_uri_t api_schedules_save_uri = {
        .uri       = "/api/schedules",
        .method    = HTTP_POST,
        .handler   = api_schedules_save_handler,
        .user_ctx  = NULL,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    err = httpd_register_uri_handler(server, &api_schedules_save_uri);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to register api_schedules_save handler: %d", err);

    httpd_uri_t api_start_uri = {
        .uri       = "/api/start",
        .method    = HTTP_POST,
        .handler   = api_start_handler,
        .user_ctx  = NULL,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    err = httpd_register_uri_handler(server, &api_start_uri);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to register api_start handler: %d", err);

    httpd_uri_t api_stop_uri = {
        .uri       = "/api/stop",
        .method    = HTTP_POST,
        .handler   = api_stop_handler,
        .user_ctx  = NULL,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    err = httpd_register_uri_handler(server, &api_stop_uri);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to register api_stop handler: %d", err);

    httpd_uri_t api_skip_uri = {
        .uri       = "/api/skip",
        .method    = HTTP_POST,
        .handler   = api_skip_handler,
        .user_ctx  = NULL,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    err = httpd_register_uri_handler(server, &api_skip_uri);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to register api_skip handler: %d", err);

    httpd_uri_t api_add_temp_uri = {
        .uri       = "/api/addTemp",
        .method    = HTTP_POST,
        .handler   = api_add_temp_handler,
        .user_ctx  = NULL,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    err = httpd_register_uri_handler(server, &api_add_temp_uri);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to register api_add_temp handler: %d", err);

    httpd_uri_t api_add_time_uri = {
        .uri       = "/api/addTime",
        .method    = HTTP_POST,
        .handler   = api_add_time_handler,
        .user_ctx  = NULL,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    err = httpd_register_uri_handler(server, &api_add_time_uri);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to register api_add_time handler: %d", err);

    httpd_uri_t manifest_uri = {
        .uri       = "/manifest.json",
        .method    = HTTP_GET,
        .handler   = manifest_handler,
        .user_ctx  = NULL,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    err = httpd_register_uri_handler(server, &manifest_uri);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to register manifest handler: %d", err);
    
    // Vite HMR handler (silence log spam)
    httpd_uri_t vite_uri = {
        .uri       = "/@vite/client",
        .method    = HTTP_GET,
        .handler   = vite_handler,
        .user_ctx  = NULL,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    err = httpd_register_uri_handler(server, &vite_uri);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to register vite handler: %d", err);
    
    // Register wildcard handler for assets
    httpd_uri_t assets_uri = {
        .uri       = "/assets/*",
        .method    = HTTP_GET,
        .handler   = static_asset_handler,
        .user_ctx  = NULL,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    err = httpd_register_uri_handler(server, &assets_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register assets handler: %d", err);
    } else {
        ESP_LOGI(TAG, "Assets handler registered for /assets/*");
    }

    // --- Captive Portal / SPA Catch-All Handlers ---
    // Register this LAST to catch everything else
    httpd_uri_t index_uri = {
        .uri       = "/*", // Catch all for SPA routing
        .method    = HTTP_GET,
        .handler   = index_handler,
        .user_ctx  = NULL,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    err = httpd_register_uri_handler(server, &index_uri);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to register index handler: %d", err);

    if (isAPMode) {
        // Redirect common captive portal endpoints
        httpd_uri_t generate_204 = {
            .uri       = "/generate_204",
            .method    = HTTP_GET,
            .handler   = redirect_handler,
            .user_ctx  = NULL,
            .is_websocket = false,
            .handle_ws_control_frames = false,
            .supported_subprotocol = NULL
        };
        httpd_register_uri_handler(server, &generate_204);

        httpd_uri_t gen_204 = {
            .uri       = "/gen_204",
            .method    = HTTP_GET,
            .handler   = redirect_handler,
            .user_ctx  = NULL,
            .is_websocket = false,
            .handle_ws_control_frames = false,
            .supported_subprotocol = NULL
        };
        httpd_register_uri_handler(server, &gen_204);
        
        httpd_uri_t hotspot_detect = {
            .uri       = "/hotspot-detect.html",
            .method    = HTTP_GET,
            .handler   = redirect_handler,
            .user_ctx  = NULL,
            .is_websocket = false,
            .handle_ws_control_frames = false,
            .supported_subprotocol = NULL
        };
        httpd_register_uri_handler(server, &hotspot_detect);

        httpd_uri_t ncsi = {
            .uri       = "/ncsi.txt",
            .method    = HTTP_GET,
            .handler   = redirect_handler,
            .user_ctx  = NULL,
            .is_websocket = false,
            .handle_ws_control_frames = false,
            .supported_subprotocol = NULL
        };
        httpd_register_uri_handler(server, &ncsi);
    }
}

void WiFiServerManager::loop() {
    uint64_t now = esp_timer_get_time() / 1000;
    if (now - lastBroadcast > 1000) {
        lastBroadcast = now;
        broadcastState();
    }
}

static const char* status_to_str(int s) {
    switch(s) {
        case 0: return "IDLE";
        case 1: return "PREHEAT";
        case 2: return "RAMP";
        case 3: return "HOLD";
        case 4: return "COOL";
        case 5: return "COMPLETE";
        case 6: return "ERROR";
        case 7: return "TUNING";
        default: return "IDLE";
    }
}

void WiFiServerManager::broadcastState() {
    if (!server) return;

    KilnState state = thermalCtrl.getState();
    DynamicJsonDocument doc(512);
    doc["temp"] = state.currentTemp;
    doc["target"] = state.targetTemp;
    doc["status"] = status_to_str((int)state.status);
    doc["firing"] = state.isFiring;
    doc["step"] = state.currentStep;
    doc["total"] = state.totalSteps;
    doc["error"] = state.errorMsg;

    std::string output;
    serializeJson(doc, output);

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)output.c_str();
    ws_pkt.len = output.length();
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    // Send to all clients
    size_t clients = 10;
    int client_fds[10];
    if (httpd_get_client_list(server, &clients, client_fds) == ESP_OK) {
        for (size_t i = 0; i < clients; i++) {
            if (httpd_ws_get_fd_info(server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_send_frame_async(server, client_fds[i], &ws_pkt);
                // Yield to other tasks to prevent WDT reset
                taskYIELD();
            }
        }
    }
}

esp_err_t WiFiServerManager::api_status_handler(httpd_req_t *req) {
    KilnState state = thermalCtrl.getState();
    DynamicJsonDocument doc(512);
    doc["temp"] = state.currentTemp;
    doc["target"] = state.targetTemp;
    doc["status"] = status_to_str((int)state.status);
    doc["firing"] = state.isFiring;
    doc["step"] = state.currentStep;
    doc["total"] = state.totalSteps;
    doc["error"] = state.errorMsg;

    std::string output;
    serializeJson(doc, output);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_schedules_handler(httpd_req_t *req) {
    // Return content of /littlefs/schedules.json
    FILE* f = fopen("/littlefs/schedules.json", "r");
    if (f == NULL) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    char chunk[512];
    size_t chunksize;
    while ((chunksize = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_schedules_save_handler(httpd_req_t *req) {
    char buf[1024];
    int ret;
    size_t remaining = req->content_len;
    
    // Open file for writing
    FILE* f = fopen("/littlefs/schedules.json", "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open schedules.json for writing");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    while (remaining > 0) {
        // Read from socket
        if ((ret = httpd_req_recv(req, buf, std::min(remaining, sizeof(buf)))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                // Retry if timeout
                continue;
            }
            fclose(f);
            return ESP_FAIL;
        }

        // Write to file
        if (fwrite(buf, 1, ret, f) != ret) {
            fclose(f);
            ESP_LOGE(TAG, "Failed to write to file");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        remaining -= ret;
    }
    fclose(f);

    ESP_LOGI(TAG, "Schedules saved successfully");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

esp_err_t WiFiServerManager::manifest_handler(httpd_req_t *req) {
    FILE* f = fopen("/littlefs/manifest.json", "r");
    if (f == NULL) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    char chunk[512];
    size_t chunksize;
    while ((chunksize = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_start_handler(httpd_req_t *req) {
    char buf[2048];
    int ret;
    int remaining = req->content_len;
    
    // If body contains schedule, load it first
    if (remaining > 0) {
        if (remaining >= sizeof(buf)) {
            ESP_LOGE(TAG, "Schedule too large");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        ret = httpd_req_recv(req, buf, remaining);
        if (ret > 0) {
            buf[ret] = '\0';
            std::string msg(buf);
            
            // Try to parse JSON properly
            DynamicJsonDocument doc(2048);
            DeserializationError error = deserializeJson(doc, msg);
            
            if (!error && (doc.containsKey("steps") || doc.containsKey("schedule"))) {
                if (doc.containsKey("schedule")) {
                    std::string scheduleStr;
                    serializeJson(doc["schedule"], scheduleStr);
                    thermalCtrl.loadSchedule(scheduleStr);
                } else {
                    thermalCtrl.loadSchedule(msg);
                }
                
                thermalCtrl.start();
                httpd_resp_sendstr(req, "{\"status\":\"started\"}");
                return ESP_OK;
            } 
            
            // If it's valid JSON but no steps, maybe it's just "start" command in JSON?
            // But for now, if it has content but no steps, we treat it as error to be safe
            // This prevents "Start" from triggering on random data
            ESP_LOGW(TAG, "Received JSON without steps, ignoring start command to be safe");
            httpd_resp_sendstr(req, "{\"error\":\"invalid_schedule\"}");
            return ESP_FAIL;
        }
    }

    // No body -> Start command
    // ONLY start if body is empty (length 0)
    // AND if a schedule is already loaded
    if (remaining == 0) {
        if (thermalCtrl.getState().totalSteps > 0) {
             ESP_LOGI(TAG, "Received Start Command (no body)");
             thermalCtrl.start();
             httpd_resp_sendstr(req, "{\"status\":\"started\"}");
        } else {
             ESP_LOGW(TAG, "Start requested but no schedule loaded");
             httpd_resp_sendstr(req, "{\"error\":\"no_schedule\"}");
        }
        return ESP_OK;
    }
    
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_stop_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Received Stop Command");
    thermalCtrl.stop("API Request");
    httpd_resp_sendstr(req, "{\"status\":\"stopped\"}");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_skip_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Received Skip Command");
    thermalCtrl.skipCurrentStep();
    httpd_resp_sendstr(req, "{\"status\":\"skip_received\"}");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_add_temp_handler(httpd_req_t *req) {
    char buf[100];
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        ESP_LOGE(TAG, "Failed to receive addTemp value");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    float tempToAdd = atof(buf);
    ESP_LOGI(TAG, "Received Add Temp Command: %.2f C", tempToAdd);
    thermalCtrl.addTemperatureToTarget(tempToAdd);
    httpd_resp_sendstr(req, "{\"status\":\"add_temp_received\", \"value\":%.2f}");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_add_time_handler(httpd_req_t *req) {
    char buf[100];
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        ESP_LOGE(TAG, "Failed to receive addTime value");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    float minutesToAdd = atof(buf);
    ESP_LOGI(TAG, "Received Add Time Command: %.2f minutes", minutesToAdd);
    thermalCtrl.addTimeToHold(minutesToAdd);
    httpd_resp_sendstr(req, "{\"status\":\"add_time_received\", \"value\":%.2f}");
    return ESP_OK;
}

WiFiServerManager wifiServer;
