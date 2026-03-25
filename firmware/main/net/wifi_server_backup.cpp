#include "net/wifi_server.h"

#include "drivers/fan_driver.h"
#include "drivers/rtc_ds3231.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "kiln_config/config.h"
#include "kiln_config/config_store.h"
#include "kiln_config/fs_utils.h"
#include "net/wifi_connection.h"
#include "cJSON.h"

#include <algorithm>
#include <string>

static constexpr const char *SCHEDULES_FILE = "/littlefs/schedules.json";
static constexpr const char *HISTORY_FILE = "/littlefs/history.json";
static constexpr const char *HISTORY_PREV_FILE = "/littlefs/history_prev.json";
static constexpr const char *EVENTS_LOG_FILE = "/littlefs/logs/events.log";
static constexpr const char *EVENTS_LOG_PREV_FILE = "/littlefs/logs/events.log.1";
static constexpr const char *AUDIT_LOG_FILE = "/littlefs/logs/audit.log";
static constexpr const char *AUDIT_LOG_PREV_FILE = "/littlefs/logs/audit.log.1";

static std::string render_json_and_delete_local(cJSON *doc) {
    if (!doc) return "{}";
    char *rendered = cJSON_PrintUnformatted(doc);
    std::string out = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);
    return out;
}

static bool read_http_body(httpd_req_t *req, std::string &out) {
    out.clear();
    if (!req || req->content_len <= 0) return true;
    out.resize((size_t)req->content_len);
    size_t off = 0;
    while (off < out.size()) {
        const int r = httpd_req_recv(req, out.data() + off, out.size() - off);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return false;
        }
        off += (size_t)r;
    }
    return true;
}

static cJSON *parse_json_or_default_object(const std::string &text) {
    if (text.empty()) return cJSON_CreateObject();
    cJSON *doc = cJSON_ParseWithLength(text.c_str(), text.size());
    if (!doc || !cJSON_IsObject(doc)) {
        if (doc) cJSON_Delete(doc);
        return cJSON_CreateObject();
    }
    return doc;
}

static cJSON *parse_json_or_default_array(const std::string &text) {
    if (text.empty()) return cJSON_CreateArray();
    cJSON *doc = cJSON_ParseWithLength(text.c_str(), text.size());
    if (!doc || !cJSON_IsArray(doc)) {
        if (doc) cJSON_Delete(doc);
        return cJSON_CreateArray();
    }
    return doc;
}

static std::string tail_text(const std::string &s, size_t max_len) {
    if (s.size() <= max_len) return s;
    return s.substr(s.size() - max_len);
}

static const char *status_to_str_local(int s) {
    switch (s) {
        case 0: return "IDLE";
        case 1: return "RUNNING";
        case 2: return "HOLD";
        case 3: return "COOLING";
        case 4: return "COMPLETE";
        case 5: return "FAULT";
        case 6: return "PAUSED";
        case 7: return "TUNING";
        default: return "IDLE";
    }
}

esp_err_t WiFiServerManager::api_backup_export_handler(httpd_req_t *req) {
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "kind", "trae_backup");
    cJSON_AddNumberToObject(doc, "schema_version", 1);
    cJSON_AddNumberToObject(doc, "ts_ms", (double)(esp_timer_get_time() / 1000ULL));

    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(doc, "fw_version", (app && app->version[0]) ? app->version : "unknown");

    cJSON_AddItemToObject(doc, "config", parse_json_or_default_object(kiln_config_load_json_config()));
    cJSON_AddItemToObject(doc, "schedules", parse_json_or_default_array(kiln_fs_read_text(SCHEDULES_FILE)));
    cJSON_AddItemToObject(doc, "history_index", parse_json_or_default_array(kiln_fs_read_text(HISTORY_FILE)));

    const std::string out = render_json_and_delete_local(doc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_backup_import_handler(httpd_req_t *req) {
    std::string body;
    if (!read_http_body(req, body)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *in = cJSON_ParseWithLength(body.c_str(), body.size());
    if (!in || !cJSON_IsObject(in)) {
        if (in) cJSON_Delete(in);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
        return ESP_OK;
    }

    bool imported_cfg = false;
    bool imported_schedules = false;
    bool imported_history = false;

    const cJSON *cfg = cJSON_GetObjectItem(in, "config");
    if (cfg && cJSON_IsObject(cfg)) {
        char *rendered = cJSON_PrintUnformatted(cfg);
        if (rendered) {
            imported_cfg = kiln_config_save_json_config(rendered);
            free(rendered);
        }
    }

    const cJSON *sched = cJSON_GetObjectItem(in, "schedules");
    if (sched && cJSON_IsArray(sched)) {
        char *rendered = cJSON_PrintUnformatted(sched);
        if (rendered) {
            imported_schedules = kiln_fs_write_text_atomic(SCHEDULES_FILE, rendered, true, true);
            free(rendered);
        }
    }

    const cJSON *hist = cJSON_GetObjectItem(in, "history_index");
    if (hist && cJSON_IsArray(hist)) {
        char *rendered = cJSON_PrintUnformatted(hist);
        if (rendered) {
            imported_history = kiln_fs_write_text_atomic(HISTORY_FILE, rendered, true, true);
            free(rendered);
        }
    }

    cJSON_Delete(in);

    if (imported_cfg) wifiServer.notifySettingsChanged("backup_import");
    if (imported_schedules) wifiServer.notifySchedulesChanged("backup_import", nullptr);

    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "ok", imported_cfg || imported_schedules || imported_history);
    cJSON_AddBoolToObject(res, "config", imported_cfg);
    cJSON_AddBoolToObject(res, "schedules", imported_schedules);
    cJSON_AddBoolToObject(res, "history_index", imported_history);
    std::string out = render_json_and_delete_local(res);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_diagnostics_bundle_handler(httpd_req_t *req) {
    const KilnState state = thermalCtrl.getState();
    const SafetyStats safety = thermalCtrl.getSafetyStats();
    const ThermalController::AutoTuneStatus tune = thermalCtrl.getAutotuneStatus();
    bool rtc_valid = false;
    (void)rtc_ds3231_is_clock_valid(&rtc_valid);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "kind", "trae_diagnostic_bundle");
    cJSON_AddNumberToObject(doc, "schema_version", 1);
    cJSON_AddNumberToObject(doc, "ts_ms", (double)(esp_timer_get_time() / 1000ULL));
    cJSON_AddStringToObject(doc, "server_url", wifi_get_server_url().c_str());

    cJSON *snapshot = cJSON_CreateObject();
    cJSON_AddNumberToObject(snapshot, "temp", state.currentTemp);
    cJSON_AddNumberToObject(snapshot, "target", state.targetTemp);
    cJSON_AddStringToObject(snapshot, "status", status_to_str_local((int)state.status));
    cJSON_AddBoolToObject(snapshot, "firing", state.isFiring);
    cJSON_AddBoolToObject(snapshot, "sensor_ok", thermalCtrl.isSensorHealthy());
    cJSON_AddBoolToObject(snapshot, "fault_active", safety.faultActive);
    cJSON_AddNumberToObject(snapshot, "fault_code", (double)safety.faultCode);
    cJSON_AddStringToObject(snapshot, "fault_reason", safety.faultReason);
    cJSON_AddBoolToObject(snapshot, "rtc_valid", rtc_valid);
    cJSON_AddNumberToObject(snapshot, "fan_power", (double)fan_driver_get_power_percent());
    cJSON_AddBoolToObject(snapshot, "fan_manual", fan_driver_get_manual());
    cJSON_AddBoolToObject(snapshot, "fan_auto", fan_driver_get_auto_enabled());

    cJSON *t = cJSON_CreateObject();
    cJSON_AddBoolToObject(t, "active", tune.active);
    cJSON_AddNumberToObject(t, "setpoint_c", (double)tune.setpointC);
    cJSON_AddNumberToObject(t, "cycles", (double)tune.cycles);
    cJSON_AddNumberToObject(t, "valid_cycles", (double)tune.valid_cycles);
    cJSON_AddNumberToObject(t, "total_cycles", (double)tune.total_cycles);
    cJSON_AddNumberToObject(t, "quality", (double)tune.quality);
    cJSON_AddNumberToObject(t, "confidence", (double)tune.confidence);
    cJSON_AddItemToObject(snapshot, "autotune", t);
    cJSON_AddItemToObject(doc, "status_snapshot", snapshot);

    cJSON_AddItemToObject(doc, "settings", parse_json_or_default_object(kiln_config_load_json_config()));
    cJSON_AddItemToObject(doc, "schedules", parse_json_or_default_array(kiln_fs_read_text(SCHEDULES_FILE)));
    cJSON_AddItemToObject(doc, "history_index", parse_json_or_default_array(kiln_fs_read_text(HISTORY_FILE)));
    cJSON_AddItemToObject(doc, "history_prev_index", parse_json_or_default_array(kiln_fs_read_text(HISTORY_PREV_FILE)));

    cJSON *logs = cJSON_CreateObject();
    cJSON_AddStringToObject(logs, "audit", tail_text(kiln_fs_read_text(AUDIT_LOG_FILE), 24000).c_str());
    cJSON_AddStringToObject(logs, "audit_prev", tail_text(kiln_fs_read_text(AUDIT_LOG_PREV_FILE), 24000).c_str());
    cJSON_AddStringToObject(logs, "events", tail_text(kiln_fs_read_text(EVENTS_LOG_FILE), 24000).c_str());
    cJSON_AddStringToObject(logs, "events_prev", tail_text(kiln_fs_read_text(EVENTS_LOG_PREV_FILE), 24000).c_str());
    cJSON_AddItemToObject(doc, "logs", logs);

    std::string out = render_json_and_delete_local(doc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out.c_str());
    return ESP_OK;
}
