#include "net/wifi_server.h"
#include "drivers/display_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "net/wifi_connection.h"
#include "net/dns_server.h"
#include "cJSON.h"
#include <string>
#include <cctype>
#include <algorithm>
#include <cmath>
#include <cstring> // For strlen, strstr
#include <cstdio>  // For FILE, fopen, snprintf
#include <unistd.h>

static const char *TAG = "SERVER";

WiFiServerManager::WiFiServerManager() {
    server = NULL;
    lastBroadcast = 0;
    isAPMode = false;
    schedulesRevision = 0;
    settingsRevision = 0;
    lastTuneActive = false;
    lastTuneCycles = -1;
}

static httpd_uri_t make_uri(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *), void *user_ctx = nullptr) {
    httpd_uri_t u{};
    u.uri = uri;
    u.method = method;
    u.handler = handler;
    u.user_ctx = user_ctx;
    return u;
}

static httpd_uri_t make_ws_uri(const char *uri, esp_err_t (*handler)(httpd_req_t *), void *user_ctx = nullptr) {
    httpd_uri_t u = make_uri(uri, HTTP_GET, handler, user_ctx);
    u.is_websocket = true;
    return u;
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

static std::string uri_path_only(const char *uri) {
    if (!uri) return {};
    const char *q = strchr(uri, '?');
    if (!q) return uri;
    return std::string(uri, (size_t)(q - uri));
}

static bool read_query_string(httpd_req_t *req, std::string &out) {
    const size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0) return false;
    out.assign(qlen + 1, '\0');
    if (httpd_req_get_url_query_str(req, out.data(), out.size()) != ESP_OK) {
        out.clear();
        return false;
    }
    out.resize(strlen(out.c_str()));
    return true;
}

static bool query_value(httpd_req_t *req, const char *key, std::string &out) {
    std::string q;
    if (!read_query_string(req, q)) return false;

    std::string buf;
    buf.assign(q.size() + 1, '\0');
    if (httpd_query_key_value(q.c_str(), key, buf.data(), buf.size()) != ESP_OK) return false;
    out.assign(buf.c_str());
    if (!out.empty()) {
        std::string decoded;
        decoded.assign(out.size() + 1, '\0');
        url_decode(decoded.data(), out.c_str());
        decoded.resize(strlen(decoded.c_str()));
        out.swap(decoded);
    }
    return true;
}

static std::string read_file_to_string(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return {};

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return {};
    }

    std::string out;
    out.resize((size_t)size);
    const size_t r = fread(out.data(), 1, out.size(), f);
    fclose(f);
    out.resize(r);
    return out;
}

static bool write_string_to_file(const char *path, const std::string &data) {
    const std::string tmp = std::string(path) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    const size_t w = fwrite(data.data(), 1, data.size(), f);
    fflush(f);
    const int fd = fileno(f);
    if (fd >= 0) (void)fsync(fd);
    fclose(f);
    if (w != data.size()) {
        (void)unlink(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path) != 0) {
        (void)unlink(tmp.c_str());
        return false;
    }
    return true;
}

static std::string uri_suffix_after(const char *uri, const char *prefix) {
    if (!uri || !prefix) return {};
    const size_t plen = strlen(prefix);
    if (strncmp(uri, prefix, plen) != 0) return {};
    const char *s = uri + plen;
    while (*s == '/') s++;
    return std::string(s);
}

static const char *get_string_or_null(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) return v->valuestring;
    return nullptr;
}

static void normalize_schedule_steps(cJSON *schedule) {
    if (!schedule || !cJSON_IsObject(schedule)) return;

    cJSON *steps = cJSON_GetObjectItem(schedule, "steps");
    if (cJSON_IsArray(steps)) return;

    cJSON *segments = cJSON_GetObjectItem(schedule, "segments");
    if (!cJSON_IsArray(segments)) return;

    cJSON_DetachItemViaPointer(schedule, segments);
    cJSON_AddItemToObject(schedule, "steps", segments);
}

static std::string ensure_schedule_name(cJSON *schedule) {
    if (!schedule || !cJSON_IsObject(schedule)) return {};

    const char *existingName = get_string_or_null(schedule, "name");
    const char *fallback = existingName;
    if (!fallback) fallback = get_string_or_null(schedule, "title");
    if (!fallback) fallback = get_string_or_null(schedule, "id");

    if (fallback) {
        if (!existingName || strcmp(existingName, fallback) != 0) {
            cJSON_DeleteItemFromObject(schedule, "name");
            cJSON_AddStringToObject(schedule, "name", fallback);
        }
        return fallback;
    }

    char generated[64];
    const uint64_t ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    snprintf(generated, sizeof(generated), "Schedule_%llu", (unsigned long long)ms);
    cJSON_DeleteItemFromObject(schedule, "name");
    cJSON_AddStringToObject(schedule, "name", generated);
    return generated;
}

static void ensure_schedule_id(cJSON *schedule) {
    if (!schedule || !cJSON_IsObject(schedule)) return;

    const char *name = get_string_or_null(schedule, "name");
    if (!name) return;

    const cJSON *idItem = cJSON_GetObjectItem(schedule, "id");
    if (cJSON_IsString(idItem) && idItem->valuestring && idItem->valuestring[0]) return;

    cJSON_DeleteItemFromObject(schedule, "id");
    cJSON_AddStringToObject(schedule, "id", name);
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

    httpd_resp_set_type(req, "text/html; charset=utf-8");

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        httpd_resp_sendstr_chunk(req, line);
    }
    fclose(f);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

esp_err_t static_asset_handler(httpd_req_t *req) {
    const std::string uriPath = uri_path_only(req->uri);

    char filepath[600];
    snprintf(filepath, sizeof(filepath), "/littlefs%s", uriPath.c_str());

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

    if (strstr(uriPath.c_str(), ".js")) httpd_resp_set_type(req, "application/javascript");
    else if (strstr(uriPath.c_str(), ".css")) httpd_resp_set_type(req, "text/css");
    else if (strstr(uriPath.c_str(), ".html")) httpd_resp_set_type(req, "text/html; charset=utf-8");
    else if (strstr(uriPath.c_str(), ".ico")) httpd_resp_set_type(req, "image/x-icon");
    else if (strstr(uriPath.c_str(), ".svg")) httpd_resp_set_type(req, "image/svg+xml");

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

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

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 64;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server, &config) == ESP_OK) {
        setupRoutes();
    }
}

void WiFiServerManager::setupRoutes() {
    httpd_uri_t save_wifi_uri = make_uri("/save_wifi", HTTP_POST, save_wifi_handler);
    httpd_register_uri_handler(server, &save_wifi_uri);

    httpd_uri_t scan_wifi_uri = make_uri("/scan_wifi", HTTP_GET, scan_wifi_handler);
    httpd_register_uri_handler(server, &scan_wifi_uri);

    httpd_uri_t scan_results_uri = make_uri("/scan_results", HTTP_GET, scan_results_handler);
    httpd_register_uri_handler(server, &scan_results_uri);

    httpd_uri_t ws_uri = make_ws_uri("/ws", ws_handler);
    httpd_register_uri_handler(server, &ws_uri);

    httpd_uri_t api_status_uri = make_uri("/api/status", HTTP_GET, api_status_handler);
    httpd_register_uri_handler(server, &api_status_uri);

    httpd_uri_t api_schedules_uri = make_uri("/api/schedules", HTTP_GET, api_schedules_handler);
    httpd_register_uri_handler(server, &api_schedules_uri);

    httpd_uri_t api_schedules_save_uri = make_uri("/api/schedules", HTTP_POST, api_schedules_save_handler);
    httpd_register_uri_handler(server, &api_schedules_save_uri);

    httpd_uri_t api_schedules_delete_uri = make_uri("/api/schedules", HTTP_DELETE, api_schedules_delete_handler);
    httpd_register_uri_handler(server, &api_schedules_delete_uri);

    httpd_uri_t api_history_list_uri = make_uri("/api/history", HTTP_GET, api_history_list_handler);
    httpd_register_uri_handler(server, &api_history_list_uri);

    httpd_uri_t api_history_detail_uri = make_uri("/api/history/*", HTTP_GET, api_history_detail_handler);
    httpd_register_uri_handler(server, &api_history_detail_uri);

    httpd_uri_t api_start_uri = make_uri("/api/start", HTTP_POST, api_start_handler);
    httpd_register_uri_handler(server, &api_start_uri);

    httpd_uri_t api_stop_uri = make_uri("/api/stop", HTTP_POST, api_stop_handler);
    httpd_register_uri_handler(server, &api_stop_uri);

    httpd_uri_t api_skip_uri = make_uri("/api/skip", HTTP_POST, api_skip_handler);
    httpd_register_uri_handler(server, &api_skip_uri);

    httpd_uri_t api_add_temp_uri = make_uri("/api/addTemp", HTTP_POST, api_add_temp_handler);
    httpd_register_uri_handler(server, &api_add_temp_uri);

    httpd_uri_t api_add_time_uri = make_uri("/api/addTime", HTTP_POST, api_add_time_handler);
    httpd_register_uri_handler(server, &api_add_time_uri);

    httpd_uri_t api_fault_clear_uri = make_uri("/api/fault/clear", HTTP_POST, api_fault_clear_handler);
    httpd_register_uri_handler(server, &api_fault_clear_uri);

    httpd_uri_t api_fault_get_uri = make_uri("/api/fault", HTTP_GET, api_fault_get_handler);
    httpd_register_uri_handler(server, &api_fault_get_uri);

    httpd_uri_t api_display_get_uri = make_uri("/api/display", HTTP_GET, api_display_get_handler);
    httpd_register_uri_handler(server, &api_display_get_uri);

    httpd_uri_t api_display_set_uri = make_uri("/api/display", HTTP_POST, api_display_set_handler);
    httpd_register_uri_handler(server, &api_display_set_uri);

    httpd_uri_t api_settings_get_uri = make_uri("/api/settings", HTTP_GET, api_settings_get_handler);
    httpd_register_uri_handler(server, &api_settings_get_uri);

    httpd_uri_t api_settings_set_uri = make_uri("/api/settings", HTTP_POST, api_settings_set_handler);
    httpd_register_uri_handler(server, &api_settings_set_uri);

    httpd_uri_t api_pid_get_uri = make_uri("/api/pid", HTTP_GET, api_pid_get_handler);
    httpd_register_uri_handler(server, &api_pid_get_uri);

    httpd_uri_t api_pid_reset_uri = make_uri("/api/pid/reset", HTTP_POST, api_pid_reset_handler);
    httpd_register_uri_handler(server, &api_pid_reset_uri);

    httpd_uri_t api_autotune_start_uri = make_uri("/api/autotune", HTTP_POST, api_autotune_start_handler);
    httpd_register_uri_handler(server, &api_autotune_start_uri);

    httpd_uri_t api_autotune_stop_uri = make_uri("/api/autotune/stop", HTTP_POST, api_autotune_stop_handler);
    httpd_register_uri_handler(server, &api_autotune_stop_uri);
 
    httpd_uri_t api_touch_get_uri = make_uri("/api/touch", HTTP_GET, api_touch_get_handler);
    httpd_register_uri_handler(server, &api_touch_get_uri);

    httpd_uri_t api_touch_spi_get_uri = make_uri("/api/touch/spi", HTTP_GET, api_touch_spi_get_handler);
    httpd_register_uri_handler(server, &api_touch_spi_get_uri);

    httpd_uri_t api_touch_spi_set_uri = make_uri("/api/touch/spi", HTTP_POST, api_touch_spi_set_handler);
    httpd_register_uri_handler(server, &api_touch_spi_set_uri);

    httpd_uri_t api_touch_transform_get_uri = make_uri("/api/touch/transform", HTTP_GET, api_touch_transform_get_handler);
    httpd_register_uri_handler(server, &api_touch_transform_get_uri);

    httpd_uri_t api_touch_transform_set_uri = make_uri("/api/touch/transform", HTTP_POST, api_touch_transform_set_handler);
    httpd_register_uri_handler(server, &api_touch_transform_set_uri);

    httpd_uri_t api_touch_cal_get_uri = make_uri("/api/touch/calibration", HTTP_GET, api_touch_calibration_get_handler);
    httpd_register_uri_handler(server, &api_touch_cal_get_uri);

    httpd_uri_t api_touch_cal_set_uri = make_uri("/api/touch/calibration", HTTP_POST, api_touch_calibration_set_handler);
    httpd_register_uri_handler(server, &api_touch_cal_set_uri);

    httpd_uri_t api_touch_pins_get_uri = make_uri("/api/touch/pins", HTTP_GET, api_touch_pins_get_handler);
    httpd_register_uri_handler(server, &api_touch_pins_get_uri);

    httpd_uri_t api_touch_pins_set_uri = make_uri("/api/touch/pins", HTTP_POST, api_touch_pins_set_handler);
    httpd_register_uri_handler(server, &api_touch_pins_set_uri);

    httpd_uri_t api_touch_probe_uri = make_uri("/api/touch/probe", HTTP_GET, api_touch_probe_handler);
    httpd_register_uri_handler(server, &api_touch_probe_uri);

    httpd_uri_t api_touch_raw_uri = make_uri("/api/touch/raw", HTTP_GET, api_touch_raw_handler);
    httpd_register_uri_handler(server, &api_touch_raw_uri);

    httpd_uri_t api_touch_stats_uri = make_uri("/api/touch/stats", HTTP_GET, api_touch_stats_handler);
    httpd_register_uri_handler(server, &api_touch_stats_uri);

    httpd_uri_t manifest_uri = make_uri("/manifest.json", HTTP_GET, manifest_handler);
    httpd_register_uri_handler(server, &manifest_uri);

    httpd_uri_t bootstrap_uri = make_uri("/bootstrap.js", HTTP_GET, static_asset_handler);
    httpd_register_uri_handler(server, &bootstrap_uri);

    httpd_uri_t touch_calib_uri = make_uri("/touch_calibration.html", HTTP_GET, static_asset_handler);
    httpd_register_uri_handler(server, &touch_calib_uri);
    
    httpd_uri_t vite_uri = make_uri("/@vite/client", HTTP_GET, vite_handler);
    httpd_register_uri_handler(server, &vite_uri);
    
    httpd_uri_t assets_uri = make_uri("/assets/*", HTTP_GET, static_asset_handler);
    httpd_register_uri_handler(server, &assets_uri);

    httpd_uri_t index_uri = make_uri("/*", HTTP_GET, index_handler);
    httpd_register_uri_handler(server, &index_uri);

    if (isAPMode) {
        httpd_uri_t generate_204 = make_uri("/generate_204", HTTP_GET, redirect_handler);
        httpd_register_uri_handler(server, &generate_204);
        httpd_uri_t gen_204 = make_uri("/gen_204", HTTP_GET, redirect_handler);
        httpd_register_uri_handler(server, &gen_204);
        httpd_uri_t hotspot_detect = make_uri("/hotspot-detect.html", HTTP_GET, redirect_handler);
        httpd_register_uri_handler(server, &hotspot_detect);
        httpd_uri_t ncsi = make_uri("/ncsi.txt", HTTP_GET, redirect_handler);
        httpd_register_uri_handler(server, &ncsi);
    }
}

esp_err_t WiFiServerManager::api_fault_clear_handler(httpd_req_t *req) {
    (void)req;
    const bool cleared = thermalCtrl.clearFault();

    const SafetyStats safety = thermalCtrl.getSafetyStats();
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "ok", cleared);
    cJSON_AddBoolToObject(doc, "fault_active", safety.faultActive);
    cJSON_AddNumberToObject(doc, "fault_code", (double)safety.faultCode);
    if (safety.faultActive) {
        cJSON_AddNumberToObject(doc, "fault_since_ms", (double)safety.faultSinceMs);
        cJSON_AddStringToObject(doc, "fault_reason", safety.faultReason);
    }

    char *rendered = cJSON_PrintUnformatted(doc);
    std::string output = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_fault_get_handler(httpd_req_t *req) {
    const SafetyStats safety = thermalCtrl.getSafetyStats();
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "fault_active", safety.faultActive);
    cJSON_AddNumberToObject(doc, "fault_code", (double)safety.faultCode);
    if (safety.faultActive) {
        cJSON_AddNumberToObject(doc, "fault_since_ms", (double)safety.faultSinceMs);
        cJSON_AddStringToObject(doc, "fault_reason", safety.faultReason);
    }

    char *rendered = cJSON_PrintUnformatted(doc);
    std::string output = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_history_list_handler(httpd_req_t *req) {
    const std::string file = read_file_to_string("/littlefs/history.json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, file.empty() ? "[]" : file.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_history_detail_handler(httpd_req_t *req) {
    const std::string id = uri_suffix_after(req->uri, "/api/history");
    if (id.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"missing_id\"}");
        return ESP_OK;
    }

    std::string path = "/littlefs/history_";
    path += id;
    path += ".json";

    const std::string file = read_file_to_string(path.c_str());
    if (file.empty()) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"not_found\"}");
        return ESP_OK;
    }

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, file.c_str());
    return ESP_OK;
}

void WiFiServerManager::loop() {
    uint64_t now = esp_timer_get_time() / 1000;
    if (now - lastBroadcast > 1000) {
        lastBroadcast = now;
        broadcastState();

        const auto tune = thermalCtrl.getAutotuneStatus();
        if (tune.active != lastTuneActive || tune.cycles != lastTuneCycles) {
            lastTuneActive = tune.active;
            lastTuneCycles = tune.cycles;
            notifyAutotuneState(tune.active ? "progress" : "inactive");
        }
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
    const ThermoStats thermo = thermalCtrl.getThermoStats();
    const SafetyStats safety = thermalCtrl.getSafetyStats();
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "ts_ms", (double)(esp_timer_get_time() / 1000ULL));
    cJSON_AddNumberToObject(doc, "temp", state.currentTemp);
    cJSON_AddNumberToObject(doc, "raw", thermo.raw);
    cJSON_AddNumberToObject(doc, "read_count", (double)thermo.readCount);
    cJSON_AddNumberToObject(doc, "target", state.targetTemp);
    cJSON_AddNumberToObject(doc, "output", thermalCtrl.getOutput());
    cJSON_AddStringToObject(doc, "status", status_to_str((int)state.status));
    cJSON_AddBoolToObject(doc, "firing", state.isFiring);
    cJSON_AddBoolToObject(doc, "sensor_ok", thermalCtrl.isSensorHealthy());
    cJSON_AddBoolToObject(doc, "fault_active", safety.faultActive);
    cJSON_AddNumberToObject(doc, "fault_code", (double)safety.faultCode);
    if (safety.faultActive) {
        cJSON_AddNumberToObject(doc, "fault_since_ms", (double)safety.faultSinceMs);
        cJSON_AddStringToObject(doc, "fault_reason", safety.faultReason);
    }
    if (state.timeRemaining >= 0) cJSON_AddNumberToObject(doc, "timeRemaining", state.timeRemaining);
    cJSON_AddNumberToObject(doc, "step", state.currentStep);
    cJSON_AddNumberToObject(doc, "total", state.totalSteps);
    cJSON_AddNumberToObject(doc, "totalSteps", state.totalSteps);
    cJSON_AddStringToObject(doc, "error", state.errorMsg.c_str());
    cJSON_AddNumberToObject(doc, "schedules_rev", (double)wifiServer.getSchedulesRevision());
    cJSON_AddNumberToObject(doc, "settings_rev", (double)wifiServer.getSettingsRevision());

    const ThermalController::AutoTuneStatus tune = thermalCtrl.getAutotuneStatus();
    cJSON *t = cJSON_CreateObject();
    cJSON_AddBoolToObject(t, "active", tune.active);
    cJSON_AddBoolToObject(t, "heater_on", tune.heaterOn);
    cJSON_AddNumberToObject(t, "setpoint_c", (double)tune.setpointC);
    cJSON_AddNumberToObject(t, "cycles", (double)tune.cycles);
    cJSON_AddNumberToObject(t, "ku", (double)tune.ku);
    cJSON_AddNumberToObject(t, "pu_s", (double)tune.pu_s);
    cJSON_AddNumberToObject(t, "kp", (double)tune.kp);
    cJSON_AddNumberToObject(t, "ki", (double)tune.ki);
    cJSON_AddNumberToObject(t, "kd", (double)tune.kd);
    cJSON_AddItemToObject(doc, "autotune", t);

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

void WiFiServerManager::notifySchedulesChanged(const char *action, const char *name) {
    schedulesRevision++;
    if (schedulesRevision == 0) schedulesRevision = 1;
    if (!server) return;

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "event", "schedules_changed");
    cJSON_AddNumberToObject(doc, "rev", (double)schedulesRevision);
    cJSON_AddNumberToObject(doc, "ts_ms", (double)(esp_timer_get_time() / 1000ULL));
    if (action && action[0]) cJSON_AddStringToObject(doc, "action", action);
    if (name && name[0]) cJSON_AddStringToObject(doc, "name", name);

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

void WiFiServerManager::notifySettingsChanged(const char *action) {
    settingsRevision++;
    if (settingsRevision == 0) settingsRevision = 1;
    if (!server) return;

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "event", "settings_changed");
    cJSON_AddNumberToObject(doc, "rev", (double)settingsRevision);
    cJSON_AddNumberToObject(doc, "ts_ms", (double)(esp_timer_get_time() / 1000ULL));
    if (action && action[0]) cJSON_AddStringToObject(doc, "action", action);

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

void WiFiServerManager::notifyAutotuneState(const char *action) {
    if (!server) return;

    const ThermalController::AutoTuneStatus tune = thermalCtrl.getAutotuneStatus();
    const SafetyStats safety = thermalCtrl.getSafetyStats();

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "event", "autotune_state");
    cJSON_AddNumberToObject(doc, "ts_ms", (double)(esp_timer_get_time() / 1000ULL));
    if (action && action[0]) cJSON_AddStringToObject(doc, "action", action);
    cJSON_AddBoolToObject(doc, "fault_active", safety.faultActive);
    cJSON_AddNumberToObject(doc, "fault_code", (double)safety.faultCode);
    if (safety.faultActive) cJSON_AddStringToObject(doc, "fault_reason", safety.faultReason);

    cJSON *t = cJSON_CreateObject();
    cJSON_AddBoolToObject(t, "active", tune.active);
    cJSON_AddBoolToObject(t, "heater_on", tune.heaterOn);
    cJSON_AddNumberToObject(t, "setpoint_c", (double)tune.setpointC);
    cJSON_AddNumberToObject(t, "cycles", (double)tune.cycles);
    cJSON_AddNumberToObject(t, "ku", (double)tune.ku);
    cJSON_AddNumberToObject(t, "pu_s", (double)tune.pu_s);
    cJSON_AddNumberToObject(t, "kp", (double)tune.kp);
    cJSON_AddNumberToObject(t, "ki", (double)tune.ki);
    cJSON_AddNumberToObject(t, "kd", (double)tune.kd);
    cJSON_AddItemToObject(doc, "autotune", t);

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
    const ThermoStats thermo = thermalCtrl.getThermoStats();
    const SafetyStats safety = thermalCtrl.getSafetyStats();
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "ts_ms", (double)(esp_timer_get_time() / 1000ULL));
    cJSON_AddNumberToObject(doc, "temp", state.currentTemp);
    cJSON_AddNumberToObject(doc, "raw", thermo.raw);
    cJSON_AddNumberToObject(doc, "read_count", (double)thermo.readCount);
    cJSON_AddNumberToObject(doc, "target", state.targetTemp);
    cJSON_AddNumberToObject(doc, "output", thermalCtrl.getOutput());
    cJSON_AddStringToObject(doc, "status", status_to_str((int)state.status));
    cJSON_AddBoolToObject(doc, "firing", state.isFiring);
    cJSON_AddBoolToObject(doc, "sensor_ok", thermalCtrl.isSensorHealthy());
    cJSON_AddBoolToObject(doc, "fault_active", safety.faultActive);
    cJSON_AddNumberToObject(doc, "fault_code", (double)safety.faultCode);
    if (safety.faultActive) {
        cJSON_AddNumberToObject(doc, "fault_since_ms", (double)safety.faultSinceMs);
        cJSON_AddStringToObject(doc, "fault_reason", safety.faultReason);
    }
    if (state.timeRemaining >= 0) cJSON_AddNumberToObject(doc, "timeRemaining", state.timeRemaining);
    cJSON_AddNumberToObject(doc, "step", state.currentStep);
    cJSON_AddNumberToObject(doc, "total", state.totalSteps);
    cJSON_AddNumberToObject(doc, "totalSteps", state.totalSteps);
    cJSON_AddStringToObject(doc, "error", state.errorMsg.c_str());
    cJSON_AddNumberToObject(doc, "schedules_rev", (double)wifiServer.getSchedulesRevision());
    cJSON_AddNumberToObject(doc, "settings_rev", (double)wifiServer.getSettingsRevision());

    const ThermalController::AutoTuneStatus tune = thermalCtrl.getAutotuneStatus();
    cJSON *t = cJSON_CreateObject();
    cJSON_AddBoolToObject(t, "active", tune.active);
    cJSON_AddBoolToObject(t, "heater_on", tune.heaterOn);
    cJSON_AddNumberToObject(t, "setpoint_c", (double)tune.setpointC);
    cJSON_AddNumberToObject(t, "cycles", (double)tune.cycles);
    cJSON_AddNumberToObject(t, "ku", (double)tune.ku);
    cJSON_AddNumberToObject(t, "pu_s", (double)tune.pu_s);
    cJSON_AddNumberToObject(t, "kp", (double)tune.kp);
    cJSON_AddNumberToObject(t, "ki", (double)tune.ki);
    cJSON_AddNumberToObject(t, "kd", (double)tune.kd);
    cJSON_AddItemToObject(doc, "autotune", t);

    char *rendered = cJSON_PrintUnformatted(doc);
    std::string output = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_schedules_handler(httpd_req_t *req) {
    // Optional: /api/schedules?name=...
    std::string name;
    (void)query_value(req, "name", name);

    const std::string file = read_file_to_string("/littlefs/schedules.json");
    if (file.empty()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, !name.empty() ? "{}" : "[]");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(file.c_str());
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, name[0] ? "{}" : "[]");
        return ESP_OK;
    }

    if (!name.empty()) {
        cJSON *match = nullptr;
        cJSON *it = nullptr;
        cJSON_ArrayForEach(it, root) {
            const cJSON *n = cJSON_GetObjectItem(it, "name");
            if (cJSON_IsString(n) && n->valuestring && strcmp(n->valuestring, name.c_str()) == 0) {
                match = it;
                break;
            }
        }

        if (!match) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "404 Not Found");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"error\":\"not_found\"}");
            return ESP_OK;
        }

        normalize_schedule_steps(match);
        ensure_schedule_id(match);
        const cJSON *steps = cJSON_GetObjectItem(match, "steps");
        const int stepsCount = cJSON_IsArray(steps) ? cJSON_GetArraySize(steps) : 0;
        cJSON_DeleteItemFromObject(match, "stepsCount");
        cJSON_AddNumberToObject(match, "stepsCount", stepsCount);

        char *rendered = cJSON_PrintUnformatted(match);
        std::string out = rendered ? rendered : "{}";
        if (rendered) free(rendered);
        cJSON_Delete(root);

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, out.c_str());
        return ESP_OK;
    }

    // Default mode: return full schedules array (so UI can edit per-program steps)
    // Also attach computed stepsCount and stable fallback name if missing.
    {
        int idx = 0;
        cJSON *it = nullptr;
        cJSON_ArrayForEach(it, root) {
            if (!cJSON_IsObject(it)) {
                idx++;
                continue;
            }

            normalize_schedule_steps(it);

            const cJSON *steps = cJSON_GetObjectItem(it, "steps");
            const int stepsCount = cJSON_IsArray(steps) ? cJSON_GetArraySize(steps) : 0;
            cJSON_DeleteItemFromObject(it, "stepsCount");
            cJSON_AddNumberToObject(it, "stepsCount", stepsCount);

            const cJSON *n = cJSON_GetObjectItem(it, "name");
            if (!cJSON_IsString(n) || !n->valuestring || !n->valuestring[0]) {
                char fallback[32];
                snprintf(fallback, sizeof(fallback), "Unnamed_%d", idx);
                cJSON_DeleteItemFromObject(it, "name");
                cJSON_AddStringToObject(it, "name", fallback);
            }
            ensure_schedule_id(it);
            idx++;
        }
    }

    char *rendered = cJSON_PrintUnformatted(root);
    std::string out = rendered ? rendered : "[]";
    if (rendered) free(rendered);
    cJSON_Delete(root);

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_schedules_save_handler(httpd_req_t *req) {
    // Accept either:
    // - a full array (legacy) -> overwrite
    // - a single schedule object -> upsert by name (matches frontend behavior)
    std::string body;
    body.resize(req->content_len);
    size_t off = 0;
    while (off < body.size()) {
        const int r = httpd_req_recv(req, body.data() + off, body.size() - off);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        off += (size_t)r;
    }

    cJSON *incoming = cJSON_ParseWithLength(body.c_str(), body.size());
    if (!incoming) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
        return ESP_OK;
    }

    cJSON *arr = nullptr;
    std::string respName;
    if (cJSON_IsArray(incoming)) {
        arr = incoming; // overwrite
        incoming = nullptr;
    } else if (cJSON_IsObject(incoming)) {
        cJSON *scheduleObj = incoming;
        cJSON *wrapped = cJSON_GetObjectItem(incoming, "schedule");
        if (cJSON_IsObject(wrapped)) {
            scheduleObj = wrapped;

            cJSON *stepsTop = cJSON_GetObjectItem(incoming, "steps");
            if (!cJSON_IsArray(stepsTop)) stepsTop = cJSON_GetObjectItem(incoming, "segments");
            if (cJSON_IsArray(stepsTop) && !cJSON_IsArray(cJSON_GetObjectItem(scheduleObj, "steps")) && !cJSON_IsArray(cJSON_GetObjectItem(scheduleObj, "segments"))) {
                cJSON_AddItemToObject(scheduleObj, "steps", cJSON_Duplicate(stepsTop, 1));
            }
        }

        normalize_schedule_steps(scheduleObj);
        respName = ensure_schedule_name(scheduleObj);
        ensure_schedule_id(scheduleObj);

        const std::string existingFile = read_file_to_string("/littlefs/schedules.json");
        cJSON *root = existingFile.empty() ? cJSON_CreateArray() : cJSON_Parse(existingFile.c_str());
        if (!root || !cJSON_IsArray(root)) {
            if (root) cJSON_Delete(root);
            root = cJSON_CreateArray();
        }

        const char *name = respName.c_str();
        bool replaced = false;
        for (int i = 0; i < cJSON_GetArraySize(root); i++) {
            cJSON *it = cJSON_GetArrayItem(root, i);
            const cJSON *n = cJSON_GetObjectItem(it, "name");
            if (cJSON_IsString(n) && n->valuestring && strcmp(n->valuestring, name) == 0) {
                cJSON_ReplaceItemInArray(root, i, cJSON_Duplicate(scheduleObj, 1));
                replaced = true;
                break;
            }
        }
        if (!replaced) cJSON_AddItemToArray(root, cJSON_Duplicate(scheduleObj, 1));

        cJSON_Delete(incoming);
        arr = root;
    } else {
        cJSON_Delete(incoming);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_payload\"}");
        return ESP_OK;
    }

    char *rendered = cJSON_PrintUnformatted(arr);
    const std::string out = rendered ? rendered : "[]";
    if (rendered) free(rendered);
    cJSON_Delete(arr);

    if (!write_string_to_file("/littlefs/schedules.json", out)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    wifiServer.notifySchedulesChanged("save", respName.empty() ? nullptr : respName.c_str());

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    if (!respName.empty()) {
        const std::string payload = std::string("{\"status\":\"ok\",\"name\":\"") + respName + "\"}";
        httpd_resp_sendstr(req, payload.c_str());
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    }
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_schedules_delete_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "DELETE /api/schedules uri=%s content_len=%d", req->uri ? req->uri : "(null)", (int)req->content_len);
    char name[64] = {0};

    int index = -1;
    {
        std::string tmp;
        if (query_value(req, "name", tmp) || query_value(req, "id", tmp) || query_value(req, "schedule", tmp)) {
            strncpy(name, tmp.c_str(), sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
        }
        if (query_value(req, "index", tmp)) {
            index = atoi(tmp.c_str());
        }
    }

    if (!name[0] && index < 0 && req->content_len > 0) {
        std::string body;
        body.resize(req->content_len);
        size_t off = 0;
        while (off < body.size()) {
            const int r = httpd_req_recv(req, body.data() + off, body.size() - off);
            if (r <= 0) {
                if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
                httpd_resp_send_500(req);
                return ESP_FAIL;
            }
            off += (size_t)r;
        }

        cJSON *root = cJSON_ParseWithLength(body.c_str(), body.size());
        if (root) {
            const char *n = nullptr;
            if (cJSON_IsObject(root)) {
                n = get_string_or_null(root, "name");
                if (!n) n = get_string_or_null(root, "title");
                if (!n) n = get_string_or_null(root, "id");
                if (!n) n = get_string_or_null(root, "schedule");
                if (n) {
                    strncpy(name, n, sizeof(name) - 1);
                    name[sizeof(name) - 1] = 0;
                }
                const cJSON *idx = cJSON_GetObjectItem(root, "index");
                if (index < 0 && cJSON_IsNumber(idx)) index = (int)idx->valuedouble;
            }
            cJSON_Delete(root);
        }
    }

    if (index < 0 && strncmp(name, "Unnamed_", 8) == 0) {
        const int parsed = atoi(name + 8);
        if (parsed >= 0) index = parsed;
        name[0] = 0;
    }

    if (!name[0] && index < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"missing_name\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "DELETE /api/schedules resolved name='%s' index=%d", name, index);

    const std::string existingFile = read_file_to_string("/littlefs/schedules.json");
    cJSON *root = existingFile.empty() ? cJSON_CreateArray() : cJSON_Parse(existingFile.c_str());
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"error\":\"not_found\"}");
        return ESP_OK;
    }

    bool removed = false;
    if (index >= 0 && index < cJSON_GetArraySize(root)) {
        cJSON_DeleteItemFromArray(root, index);
        removed = true;
    } else if (name[0]) {
        for (int i = cJSON_GetArraySize(root) - 1; i >= 0; i--) {
            cJSON *it = cJSON_GetArrayItem(root, i);
            const cJSON *n = cJSON_GetObjectItem(it, "name");
            if (cJSON_IsString(n) && n->valuestring && strcmp(n->valuestring, name) == 0) {
                cJSON_DeleteItemFromArray(root, i);
                removed = true;
            }
        }
    }

    if (!removed) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"error\":\"not_found\"}");
        return ESP_OK;
    }

    char *rendered = cJSON_PrintUnformatted(root);
    const std::string out = rendered ? rendered : "[]";
    if (rendered) free(rendered);
    cJSON_Delete(root);

    if (!write_string_to_file("/littlefs/schedules.json", out)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    wifiServer.notifySchedulesChanged("delete", name[0] ? name : nullptr);

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_display_get_handler(httpd_req_t *req) {
    bool mx = false, my = false;
    display_driver_get_mirror(&mx, &my);
    int xoff = 0, yoff = 0;
    display_driver_get_offset(&xoff, &yoff);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "mirror_x", mx);
    cJSON_AddBoolToObject(doc, "mirror_y", my);
    cJSON_AddNumberToObject(doc, "x_offset", xoff);
    cJSON_AddNumberToObject(doc, "y_offset", yoff);

    char *rendered = cJSON_PrintUnformatted(doc);
    std::string output = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_display_set_handler(httpd_req_t *req) {
    char buf[128];
    int ret = 0;
    size_t remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
        return ESP_OK;
    }

    bool mx = false, my = false;
    int xoff = 0, yoff = 0;
    display_driver_get_mirror(&mx, &my);
    display_driver_get_offset(&xoff, &yoff);

    const cJSON *mxItem = cJSON_GetObjectItem(root, "mirror_x");
    const cJSON *myItem = cJSON_GetObjectItem(root, "mirror_y");
    const cJSON *xoItem = cJSON_GetObjectItem(root, "x_offset");
    const cJSON *yoItem = cJSON_GetObjectItem(root, "y_offset");
    if (cJSON_IsBool(mxItem)) mx = cJSON_IsTrue(mxItem);
    if (cJSON_IsBool(myItem)) my = cJSON_IsTrue(myItem);
    if (cJSON_IsNumber(xoItem)) xoff = (int)xoItem->valuedouble;
    if (cJSON_IsNumber(yoItem)) yoff = (int)yoItem->valuedouble;

    display_driver_set_mirror(mx, my);
    if (!display_driver_set_offset(xoff, yoff)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_offset\"}");
        return ESP_OK;
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_settings_get_handler(httpd_req_t *req) {
    cJSON *root = nullptr;
    const std::string existing = read_file_to_string("/littlefs/config.json");
    if (!existing.empty()) root = cJSON_Parse(existing.c_str());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        root = cJSON_CreateObject();
    }

    cJSON_DeleteItemFromObject(root, "offset");
    cJSON_DeleteItemFromObject(root, "temp_offset_c");
    cJSON_AddNumberToObject(root, "temp_offset_c", (double)thermalCtrl.getTemperatureOffset());

    char *rendered = cJSON_PrintUnformatted(root);
    std::string output = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(root);

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_settings_set_handler(httpd_req_t *req) {
    std::string body;
    body.resize(req->content_len);
    size_t off = 0;
    while (off < body.size()) {
        const int r = httpd_req_recv(req, body.data() + off, body.size() - off);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        off += (size_t)r;
    }

    cJSON *incoming = cJSON_ParseWithLength(body.c_str(), body.size());
    if (!incoming || !cJSON_IsObject(incoming)) {
        if (incoming) cJSON_Delete(incoming);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
        return ESP_OK;
    }

    // Load existing config (merge)
    cJSON *root = nullptr;
    const std::string existing = read_file_to_string("/littlefs/config.json");
    if (!existing.empty()) root = cJSON_Parse(existing.c_str());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        root = cJSON_CreateObject();
    }

    const cJSON *offset = cJSON_GetObjectItem(incoming, "offset");
    if (!cJSON_IsNumber(offset)) offset = cJSON_GetObjectItem(incoming, "temp_offset_c");
    if (cJSON_IsNumber(offset)) {
        (void)thermalCtrl.setTemperatureOffset((float)offset->valuedouble);
    }

    const cJSON *ssrCycles = cJSON_GetObjectItem(incoming, "ssrCycles");
    if (cJSON_IsNumber(ssrCycles)) {
        cJSON_DeleteItemFromObject(root, "ssrCycles");
        cJSON_AddNumberToObject(root, "ssrCycles", (double)ssrCycles->valuedouble);
    }

    const cJSON *wattage = cJSON_GetObjectItem(incoming, "wattage");
    if (cJSON_IsNumber(wattage)) {
        cJSON_DeleteItemFromObject(root, "wattage");
        cJSON_AddNumberToObject(root, "wattage", (double)wattage->valuedouble);
    }

    const cJSON *cost = cJSON_GetObjectItem(incoming, "costPerKwh");
    if (cJSON_IsNumber(cost)) {
        cJSON_DeleteItemFromObject(root, "costPerKwh");
        cJSON_AddNumberToObject(root, "costPerKwh", (double)cost->valuedouble);
    }

    const cJSON *currency = cJSON_GetObjectItem(incoming, "currency");
    if (cJSON_IsString(currency) && currency->valuestring) {
        cJSON_DeleteItemFromObject(root, "currency");
        cJSON_AddStringToObject(root, "currency", currency->valuestring);
    }

    const cJSON *zones = cJSON_GetObjectItem(incoming, "zones");
    if (cJSON_IsNumber(zones)) {
        cJSON_DeleteItemFromObject(root, "zones");
        cJSON_AddNumberToObject(root, "zones", (double)zones->valuedouble);
    }

    // Always persist the offset in a stable key
    cJSON_DeleteItemFromObject(root, "temp_offset_c");
    cJSON_AddNumberToObject(root, "temp_offset_c", (double)thermalCtrl.getTemperatureOffset());

    char *rendered = cJSON_PrintUnformatted(root);
    const std::string out = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(root);
    cJSON_Delete(incoming);

    if (!write_string_to_file("/littlefs/config.json", out)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    wifiServer.notifySettingsChanged("save");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_pid_get_handler(httpd_req_t *req) {
    const ThermalController::PidTunings pid = thermalCtrl.getPidTunings();

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "kp", pid.kp);
    cJSON_AddNumberToObject(doc, "ki", pid.ki);
    cJSON_AddNumberToObject(doc, "kd", pid.kd);
    cJSON_AddNumberToObject(doc, "kp_default", pid.kp_default);
    cJSON_AddNumberToObject(doc, "ki_default", pid.ki_default);
    cJSON_AddNumberToObject(doc, "kd_default", pid.kd_default);
    cJSON_AddNumberToObject(doc, "temp_offset_c", (double)pid.temp_offset_c);
    const bool is_default = (fabs(pid.kp - pid.kp_default) < 1e-9) &&
                            (fabs(pid.ki - pid.ki_default) < 1e-9) &&
                            (fabs(pid.kd - pid.kd_default) < 1e-9);
    cJSON_AddBoolToObject(doc, "is_default", is_default);

    char *rendered = cJSON_PrintUnformatted(doc);
    std::string output = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_pid_reset_handler(httpd_req_t *req) {
    (void)req;
    const bool ok = thermalCtrl.resetPidToDefaults();
    if (ok) wifiServer.notifySettingsChanged("pid_reset");

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    if (ok) {
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    } else {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"error\":\"busy\"}");
    }
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_autotune_start_handler(httpd_req_t *req) {
    std::string body;
    body.resize(req->content_len);
    size_t off = 0;
    while (off < body.size()) {
        const int r = httpd_req_recv(req, body.data() + off, body.size() - off);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        off += (size_t)r;
    }

    float target = 600.0f;
    if (!body.empty()) {
        cJSON *root = cJSON_ParseWithLength(body.c_str(), body.size());
        if (root) {
            const cJSON *t = cJSON_GetObjectItem(root, "temp");
            if (cJSON_IsNumber(t)) target = (float)t->valuedouble;
            cJSON_Delete(root);
        }
    }

    const bool ok = thermalCtrl.startAutotune(target);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    if (ok) {
        wifiServer.notifyAutotuneState("start");
        httpd_resp_sendstr(req, "{\"status\":\"started\"}");
    } else {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"error\":\"busy_or_fault\"}");
    }
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_autotune_stop_handler(httpd_req_t *req) {
    (void)req;
    thermalCtrl.stopAutotune("Autotune stopped");
    wifiServer.notifyAutotuneState("stop");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"stopped\"}");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_touch_get_handler(httpd_req_t *req) {
    uint16_t rx = 0, ry = 0, z1 = 0;
    bool pressed = false;
    display_driver_get_touch_debug(&rx, &ry, &z1, &pressed);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "pressed", pressed);
    cJSON_AddNumberToObject(doc, "raw_x", rx);
    cJSON_AddNumberToObject(doc, "raw_y", ry);
    cJSON_AddNumberToObject(doc, "z1", z1);

    char *rendered = cJSON_PrintUnformatted(doc);
    std::string output = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_touch_spi_get_handler(httpd_req_t *req) {
    int mode = 0, hz = 0;
    display_driver_get_touch_spi(&mode, &hz);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "mode", mode);
    cJSON_AddNumberToObject(doc, "hz", hz);

    char *rendered = cJSON_PrintUnformatted(doc);
    std::string output = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_touch_spi_set_handler(httpd_req_t *req) {
    char buf[128];
    int ret = 0;
    size_t remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
        return ESP_OK;
    }

    int mode = 0, hz = 0;
    display_driver_get_touch_spi(&mode, &hz);

    const cJSON *modeItem = cJSON_GetObjectItem(root, "mode");
    const cJSON *hzItem = cJSON_GetObjectItem(root, "hz");
    if (cJSON_IsNumber(modeItem)) mode = (int)modeItem->valuedouble;
    if (cJSON_IsNumber(hzItem)) hz = (int)hzItem->valuedouble;

    const bool ok = display_driver_set_touch_spi(mode, hz);
    cJSON_Delete(root);

    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_params\"}");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_touch_transform_get_handler(httpd_req_t *req) {
    bool swap_xy = false;
    bool mirror_x = false;
    bool mirror_y = false;
    display_driver_get_touch_transform(&swap_xy, &mirror_x, &mirror_y);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "swap_xy", swap_xy);
    cJSON_AddBoolToObject(doc, "mirror_x", mirror_x);
    cJSON_AddBoolToObject(doc, "mirror_y", mirror_y);

    char *rendered = cJSON_PrintUnformatted(doc);
    std::string out = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_touch_transform_set_handler(httpd_req_t *req) {
    std::string body;
    body.resize(req->content_len);
    size_t off = 0;
    while (off < body.size()) {
        const int r = httpd_req_recv(req, body.data() + off, body.size() - off);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        off += (size_t)r;
    }

    cJSON *root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_payload\"}");
        return ESP_OK;
    }

    bool swap_xy = false;
    bool mirror_x = false;
    bool mirror_y = false;
    display_driver_get_touch_transform(&swap_xy, &mirror_x, &mirror_y);

    const cJSON *sx = cJSON_GetObjectItem(root, "swap_xy");
    const cJSON *mx = cJSON_GetObjectItem(root, "mirror_x");
    const cJSON *my = cJSON_GetObjectItem(root, "mirror_y");
    if (cJSON_IsBool(sx)) swap_xy = cJSON_IsTrue(sx);
    if (cJSON_IsBool(mx)) mirror_x = cJSON_IsTrue(mx);
    if (cJSON_IsBool(my)) mirror_y = cJSON_IsTrue(my);

    cJSON_Delete(root);

    if (!display_driver_set_touch_transform(swap_xy, mirror_x, mirror_y)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "status", "ok");
    cJSON_AddBoolToObject(doc, "swap_xy", swap_xy);
    cJSON_AddBoolToObject(doc, "mirror_x", mirror_x);
    cJSON_AddBoolToObject(doc, "mirror_y", mirror_y);
    char *rendered = cJSON_PrintUnformatted(doc);
    std::string out = rendered ? rendered : "{\"status\":\"ok\"}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_touch_calibration_get_handler(httpd_req_t *req) {
    bool enabled = false;
    uint16_t left = 0, right = 0, top = 0, bottom = 0;
    display_driver_get_touch_calibration(&enabled, &left, &right, &top, &bottom);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "enabled", enabled);
    cJSON_AddNumberToObject(doc, "left", (int)left);
    cJSON_AddNumberToObject(doc, "right", (int)right);
    cJSON_AddNumberToObject(doc, "top", (int)top);
    cJSON_AddNumberToObject(doc, "bottom", (int)bottom);

    char *rendered = cJSON_PrintUnformatted(doc);
    std::string out = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_touch_calibration_set_handler(httpd_req_t *req) {
    std::string body;
    body.resize(req->content_len);
    size_t off = 0;
    while (off < body.size()) {
        const int r = httpd_req_recv(req, body.data() + off, body.size() - off);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        off += (size_t)r;
    }

    cJSON *root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_payload\"}");
        return ESP_OK;
    }

    const cJSON *reset = cJSON_GetObjectItem(root, "reset");
    if (cJSON_IsBool(reset) && cJSON_IsTrue(reset)) {
        cJSON_Delete(root);
        display_driver_reset_touch_calibration();
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Pragma", "no-cache");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"reset\":true}");
        return ESP_OK;
    }

    bool enabled = false;
    uint16_t left = 0, right = 0, top = 0, bottom = 0;
    display_driver_get_touch_calibration(&enabled, &left, &right, &top, &bottom);

    const cJSON *en = cJSON_GetObjectItem(root, "enabled");
    const cJSON *l = cJSON_GetObjectItem(root, "left");
    const cJSON *r = cJSON_GetObjectItem(root, "right");
    const cJSON *t = cJSON_GetObjectItem(root, "top");
    const cJSON *b = cJSON_GetObjectItem(root, "bottom");
    if (cJSON_IsBool(en)) enabled = cJSON_IsTrue(en);
    if (cJSON_IsNumber(l)) left = (uint16_t)l->valuedouble;
    if (cJSON_IsNumber(r)) right = (uint16_t)r->valuedouble;
    if (cJSON_IsNumber(t)) top = (uint16_t)t->valuedouble;
    if (cJSON_IsNumber(b)) bottom = (uint16_t)b->valuedouble;
    cJSON_Delete(root);

    if (!display_driver_set_touch_calibration(enabled, left, right, top, bottom)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_params\"}");
        return ESP_OK;
    }

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "status", "ok");
    cJSON_AddBoolToObject(doc, "enabled", enabled);
    cJSON_AddNumberToObject(doc, "left", (int)left);
    cJSON_AddNumberToObject(doc, "right", (int)right);
    cJSON_AddNumberToObject(doc, "top", (int)top);
    cJSON_AddNumberToObject(doc, "bottom", (int)bottom);
    char *rendered = cJSON_PrintUnformatted(doc);
    std::string out = rendered ? rendered : "{\"status\":\"ok\"}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_touch_pins_get_handler(httpd_req_t *req) {
    int sclk = 0, mosi = 0, miso = 0;
    display_driver_get_touch_pins(&sclk, &mosi, &miso);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "sclk", sclk);
    cJSON_AddNumberToObject(doc, "mosi", mosi);
    cJSON_AddNumberToObject(doc, "miso", miso);

    char *rendered = cJSON_PrintUnformatted(doc);
    std::string output = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_touch_pins_set_handler(httpd_req_t *req) {
    char buf[256];
    int ret = 0;
    size_t remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
        return ESP_OK;
    }

    int sclk = 0, mosi = 0, miso = 0;
    display_driver_get_touch_pins(&sclk, &mosi, &miso);

    const cJSON *sclkItem = cJSON_GetObjectItem(root, "sclk");
    const cJSON *mosiItem = cJSON_GetObjectItem(root, "mosi");
    const cJSON *misoItem = cJSON_GetObjectItem(root, "miso");
    if (cJSON_IsNumber(sclkItem)) sclk = (int)sclkItem->valuedouble;
    if (cJSON_IsNumber(mosiItem)) mosi = (int)mosiItem->valuedouble;
    if (cJSON_IsNumber(misoItem)) miso = (int)misoItem->valuedouble;

    const bool ok = display_driver_set_touch_pins(sclk, mosi, miso);
    cJSON_Delete(root);

    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_params\"}");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"rebooting\":true}");

    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_touch_probe_handler(httpd_req_t *req) {
    uint16_t rx = 0, ry = 0, z1 = 0;
    const bool pressed = display_driver_touch_probe(&rx, &ry, &z1);

    uint8_t bz1[3] = {0}, bx[3] = {0}, by[3] = {0};
    display_driver_get_touch_last_bytes(bz1, bx, by);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "pressed", pressed);
    cJSON_AddNumberToObject(doc, "raw_x", rx);
    cJSON_AddNumberToObject(doc, "raw_y", ry);
    cJSON_AddNumberToObject(doc, "z1", z1);

    const std::string rx_z1 = std::to_string(bz1[0]) + "," + std::to_string(bz1[1]) + "," + std::to_string(bz1[2]);
    const std::string rx_x = std::to_string(bx[0]) + "," + std::to_string(bx[1]) + "," + std::to_string(bx[2]);
    const std::string rx_y = std::to_string(by[0]) + "," + std::to_string(by[1]) + "," + std::to_string(by[2]);
    cJSON_AddStringToObject(doc, "rx_z1", rx_z1.c_str());
    cJSON_AddStringToObject(doc, "rx_x", rx_x.c_str());
    cJSON_AddStringToObject(doc, "rx_y", rx_y.c_str());

    char *rendered = cJSON_PrintUnformatted(doc);
    std::string output = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_touch_raw_handler(httpd_req_t *req) {
    uint16_t rx = 0, ry = 0, z1 = 0;
    const bool pressed = display_driver_touch_probe_raw(&rx, &ry, &z1);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "pressed", pressed);
    cJSON_AddNumberToObject(doc, "raw_x", rx);
    cJSON_AddNumberToObject(doc, "raw_y", ry);
    cJSON_AddNumberToObject(doc, "z1", z1);

    char *rendered = cJSON_PrintUnformatted(doc);
    std::string output = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_touch_stats_handler(httpd_req_t *req) {
    uint32_t cb = 0;
    display_driver_get_touch_stats(&cb);

    uint16_t rx = 0, ry = 0, z1 = 0;
    bool pressed = false;
    display_driver_get_touch_debug(&rx, &ry, &z1, &pressed);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "read_cb_count", (double)cb);
    cJSON_AddBoolToObject(doc, "pressed_cached", pressed);
    cJSON_AddNumberToObject(doc, "raw_x_cached", rx);
    cJSON_AddNumberToObject(doc, "raw_y_cached", ry);
    cJSON_AddNumberToObject(doc, "z1_cached", z1);

    char *rendered = cJSON_PrintUnformatted(doc);
    std::string output = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
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
            if (!steps) steps = cJSON_GetObjectItem(root, "segments");

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
