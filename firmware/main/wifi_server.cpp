#include "wifi_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "wifi_connection.h"
#include "dns_server.h"
#include "cJSON.h"
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
            if (a >= 'a') a -= 'a'-'A';
            if (a >= 'A') a -= ('A' - 10); else a -= '0';
            if (b >= 'a') b -= 'a'-'A';
            if (b >= 'A') b -= ('A' - 10); else b -= '0';
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
    if (strncmp(req->uri, "/api/", 5) == 0 || strncmp(req->uri, "/assets/", 8) == 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

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

    httpd_resp_set_type(req, "text/html");

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        httpd_resp_sendstr_chunk(req, line);
    }
    fclose(f);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

esp_err_t static_asset_handler(httpd_req_t *req) {
    char filepath[600];
    snprintf(filepath, sizeof(filepath), "/littlefs%s", req->uri);

    FILE* f = fopen(filepath, "r");
    if (f == NULL) {
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

    if (strstr(req->uri, ".js")) httpd_resp_set_type(req, "application/javascript");
    else if (strstr(req->uri, ".css")) httpd_resp_set_type(req, "text/css");
    else if (strstr(req->uri, ".ico")) httpd_resp_set_type(req, "image/x-icon");
    else if (strstr(req->uri, ".svg")) httpd_resp_set_type(req, "image/svg+xml");

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

esp_err_t redirect_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t vite_handler(httpd_req_t *req) {
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t WiFiServerManager::scan_wifi_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Starting async WiFi scan");
    wifi_start_scan();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"scanning_started\"}");
    return ESP_OK;
}

esp_err_t WiFiServerManager::scan_results_handler(httpd_req_t *req) {
    if (wifi_is_scan_done()) {
        std::string json = wifi_get_scanned_networks();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json.c_str());
        ESP_LOGI(TAG, "Sent scan results");
    } else {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"scanning\"}");
    }
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
    if (ret <= 0) { /* Handle error */ return ESP_FAIL; }
    buf[ret] = '\0';

    char ssid[33] = {0}, password[65] = {0};
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

    ESP_LOGI(TAG, "Saving WiFi: SSID='%s'", ssid);
    wifi_save_creds(ssid, password);

    httpd_resp_sendstr(req, "WiFi Saved. Rebooting...");
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

esp_err_t WiFiServerManager::ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    if (httpd_ws_recv_frame(req, &ws_pkt, 0) != ESP_OK) return ESP_FAIL;

    if (ws_pkt.len) {
        buf = (uint8_t*)calloc(1, ws_pkt.len + 1);
        if (!buf) return ESP_ERR_NO_MEM;
        ws_pkt.payload = buf;
        if (httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len) != ESP_OK) {
            free(buf);
            return ESP_FAIL;
        }
        
        std::string msg((char*)buf);

        cJSON *root = cJSON_Parse(msg.c_str());
        if (root) {
            const cJSON *steps = cJSON_GetObjectItem(root, "steps");
            const cJSON *command = cJSON_GetObjectItem(root, "command");
            const cJSON *action = cJSON_GetObjectItem(root, "action");

            if (steps) {
                thermalCtrl.loadSchedule(msg);
            } else if ((cJSON_IsString(command) && command->valuestring && strcmp(command->valuestring, "stop") == 0) ||
                       (cJSON_IsString(action) && action->valuestring && strcmp(action->valuestring, "stop") == 0)) {
                thermalCtrl.stop("Web Request");
            }

            cJSON_Delete(root);
        } else if (msg == "stop" || msg == "STOP") {
            thermalCtrl.stop("Web Request");
        }
    }

    free(buf);
    return ESP_OK;
}

void WiFiServerManager::begin() {
    if (wifi_is_configured()) {
        if (wifi_init_sta()) isAPMode = false;
        else {
            wifi_init_softap();
            start_dns_server();
            isAPMode = true;
        }
    } else {
        wifi_init_softap();
        start_dns_server();
        isAPMode = true;
    }

    thermalCtrl.stop("System Boot");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 22;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server, &config) == ESP_OK) {
        setupRoutes();
    }
}

void WiFiServerManager::setupRoutes() {
    httpd_uri_t save_wifi_uri = { .uri = "/save_wifi", .method = HTTP_POST, .handler = save_wifi_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &save_wifi_uri);

    httpd_uri_t scan_wifi_uri = { .uri = "/scan_wifi", .method = HTTP_GET, .handler = scan_wifi_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &scan_wifi_uri);

    httpd_uri_t scan_results_uri = { .uri = "/scan_results", .method = HTTP_GET, .handler = scan_results_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &scan_results_uri);

    httpd_uri_t ws_uri = { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .user_ctx = NULL, .is_websocket = true };
    httpd_register_uri_handler(server, &ws_uri);

    httpd_uri_t api_status_uri = { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &api_status_uri);

    httpd_uri_t api_schedules_uri = { .uri = "/api/schedules", .method = HTTP_GET, .handler = api_schedules_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &api_schedules_uri);

    httpd_uri_t api_schedules_save_uri = { .uri = "/api/schedules", .method = HTTP_POST, .handler = api_schedules_save_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &api_schedules_save_uri);

    httpd_uri_t api_start_uri = { .uri = "/api/start", .method = HTTP_POST, .handler = api_start_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &api_start_uri);

    httpd_uri_t api_stop_uri = { .uri = "/api/stop", .method = HTTP_POST, .handler = api_stop_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &api_stop_uri);

    httpd_uri_t api_skip_uri = { .uri = "/api/skip", .method = HTTP_POST, .handler = api_skip_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &api_skip_uri);

    httpd_uri_t api_add_temp_uri = { .uri = "/api/addTemp", .method = HTTP_POST, .handler = api_add_temp_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &api_add_temp_uri);

    httpd_uri_t api_add_time_uri = { .uri = "/api/addTime", .method = HTTP_POST, .handler = api_add_time_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &api_add_time_uri);

    httpd_uri_t manifest_uri = { .uri = "/manifest.json", .method = HTTP_GET, .handler = manifest_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &manifest_uri);
    
    httpd_uri_t vite_uri = { .uri = "/@vite/client", .method = HTTP_GET, .handler = vite_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &vite_uri);
    
    httpd_uri_t assets_uri = { .uri = "/assets/*", .method = HTTP_GET, .handler = static_asset_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &assets_uri);

    httpd_uri_t index_uri = { .uri = "/*", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &index_uri);

    if (isAPMode) {
        httpd_uri_t generate_204 = { .uri = "/generate_204", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &generate_204);
        httpd_uri_t gen_204 = { .uri = "/gen_204", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &gen_204);
        httpd_uri_t hotspot_detect = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &hotspot_detect);
        httpd_uri_t ncsi = { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = redirect_handler, .user_ctx = NULL };
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
        case 0: return "IDLE"; case 1: return "PREHEAT"; case 2: return "RAMP";
        case 3: return "HOLD"; case 4: return "COOL"; case 5: return "COMPLETE";
        case 6: return "ERROR"; case 7: return "TUNING"; default: return "IDLE";
    }
}

void WiFiServerManager::broadcastState() {
    if (!server) return;

    KilnState state = thermalCtrl.getState();
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "temp", state.currentTemp);
    cJSON_AddNumberToObject(doc, "target", state.targetTemp);
    cJSON_AddStringToObject(doc, "status", status_to_str((int)state.status));
    cJSON_AddBoolToObject(doc, "firing", state.isFiring);
    cJSON_AddNumberToObject(doc, "step", state.currentStep);
    cJSON_AddNumberToObject(doc, "total", state.totalSteps);
    cJSON_AddStringToObject(doc, "error", state.errorMsg.c_str());

    char *rendered = cJSON_PrintUnformatted(doc);
    std::string output = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)output.c_str();
    ws_pkt.len = output.length();
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    size_t clients = 10;
    int client_fds[10];
    if (httpd_get_client_list(server, &clients, client_fds) == ESP_OK) {
        for (size_t i = 0; i < clients; i++) {
            if (httpd_ws_get_fd_info(server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_send_frame_async(server, client_fds[i], &ws_pkt);
                taskYIELD();
            }
        }
    }
}

esp_err_t WiFiServerManager::api_status_handler(httpd_req_t *req) {
    KilnState state = thermalCtrl.getState();
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "temp", state.currentTemp);
    cJSON_AddNumberToObject(doc, "target", state.targetTemp);
    cJSON_AddStringToObject(doc, "status", status_to_str((int)state.status));
    cJSON_AddBoolToObject(doc, "firing", state.isFiring);
    cJSON_AddNumberToObject(doc, "step", state.currentStep);
    cJSON_AddNumberToObject(doc, "total", state.totalSteps);
    cJSON_AddStringToObject(doc, "error", state.errorMsg.c_str());

    char *rendered = cJSON_PrintUnformatted(doc);
    std::string output = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_schedules_handler(httpd_req_t *req) {
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
    
    FILE* f = fopen("/littlefs/schedules.json", "w");
    if (f == NULL) { httpd_resp_send_500(req); return ESP_FAIL; }

    while (remaining > 0) {
        if ((ret = httpd_req_recv(req, buf, std::min(remaining, sizeof(buf)))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            fclose(f);
            return ESP_FAIL;
        }
        if (fwrite(buf, 1, ret, f) != ret) {
            fclose(f);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        remaining -= ret;
    }
    fclose(f);

    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

esp_err_t WiFiServerManager::manifest_handler(httpd_req_t *req) {
    FILE* f = fopen("/littlefs/manifest.json", "r");
    if (f == NULL) { httpd_resp_send_404(req); return ESP_FAIL; }

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
    int ret, remaining = req->content_len;
    
    if (remaining > 0) {
        if (remaining >= sizeof(buf)) { httpd_resp_send_500(req); return ESP_FAIL; }
        ret = httpd_req_recv(req, buf, remaining);
        if (ret > 0) {
            buf[ret] = '\0';
            std::string msg(buf);

            cJSON *root = cJSON_Parse(msg.c_str());
            if (!root) {
                httpd_resp_sendstr(req, "{\"error\":\"invalid_schedule\"}");
                return ESP_OK;
            }

            cJSON *schedule = cJSON_GetObjectItem(root, "schedule");
            cJSON *steps = cJSON_GetObjectItem(root, "steps");

            if (schedule && cJSON_IsObject(schedule)) {
                char *rendered = cJSON_PrintUnformatted(schedule);
                if (rendered) {
                    thermalCtrl.loadSchedule(rendered);
                    free(rendered);
                } else {
                    cJSON_Delete(root);
                    httpd_resp_sendstr(req, "{\"error\":\"invalid_schedule\"}");
                    return ESP_OK;
                }
            } else if (steps) {
                thermalCtrl.loadSchedule(msg);
            } else {
                cJSON_Delete(root);
                httpd_resp_sendstr(req, "{\"error\":\"invalid_schedule\"}");
                return ESP_OK;
            }

            cJSON_Delete(root);
            thermalCtrl.start();
            httpd_resp_sendstr(req, "{\"status\":\"started\"}");
        } 
    } else {
        if (thermalCtrl.getState().totalSteps > 0) {
             thermalCtrl.start();
             httpd_resp_sendstr(req, "{\"status\":\"started\"}");
        } else {
             httpd_resp_sendstr(req, "{\"error\":\"no_schedule\"}");
        }
    }
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_stop_handler(httpd_req_t *req) {
    thermalCtrl.stop("API Request");
    httpd_resp_sendstr(req, "{\"status\":\"stopped\"}");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_skip_handler(httpd_req_t *req) {
    thermalCtrl.skipCurrentStep();
    httpd_resp_sendstr(req, "{\"status\":\"skip_received\"}");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_add_temp_handler(httpd_req_t *req) {
    char buf[100];
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret > 0) {
        buf[ret] = '\0';
        thermalCtrl.addTemperatureToTarget(atof(buf));
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    }
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_add_time_handler(httpd_req_t *req) {
    char buf[100];
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret > 0) {
        buf[ret] = '\0';
        thermalCtrl.addTimeToHold(atof(buf));
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    }
    return ESP_OK;
}

WiFiServerManager wifiServer;
