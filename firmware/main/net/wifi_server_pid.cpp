#include "net/wifi_server.h"

#include "app/device_commands.h"
#include "cJSON.h"

#include <cmath>
#include <cstdlib>
#include <string>

static bool get_number_like_pid(const cJSON *obj, const char *key, double &out) {
    if (!obj || !key) return false;
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(v)) {
        out = v->valuedouble;
        return true;
    }
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
        char *end = nullptr;
        const double d = std::strtod(v->valuestring, &end);
        if (end && end != v->valuestring) {
            out = d;
            return true;
        }
    }
    return false;
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
    cJSON_AddNumberToObject(doc, "low_kp", pid.low_kp);
    cJSON_AddNumberToObject(doc, "low_ki", pid.low_ki);
    cJSON_AddNumberToObject(doc, "low_kd", pid.low_kd);
    cJSON_AddNumberToObject(doc, "high_kp", pid.high_kp);
    cJSON_AddNumberToObject(doc, "high_ki", pid.high_ki);
    cJSON_AddNumberToObject(doc, "high_kd", pid.high_kd);
    const bool is_default = (fabs(pid.kp - pid.kp_default) < 1e-9) &&
                            (fabs(pid.ki - pid.ki_default) < 1e-9) &&
                            (fabs(pid.kd - pid.kd_default) < 1e-9);
    cJSON_AddBoolToObject(doc, "is_default", is_default);
    cJSON_AddNumberToObject(doc, "autotune_target_c", (double)thermalCtrl.getAutotuneTargetC());
    cJSON_AddNumberToObject(doc, "autotuneTargetC", (double)thermalCtrl.getAutotuneTargetC());

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

esp_err_t WiFiServerManager::api_pid_post_handler(httpd_req_t *req) {
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
        httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
        return ESP_OK;
    }

    double low_kp = 0.0, low_ki = 0.0, low_kd = 0.0;
    double high_kp = 0.0, high_ki = 0.0, high_kd = 0.0;
    thermalCtrl.getPidProfileTunings(false, &low_kp, &low_ki, &low_kd);
    thermalCtrl.getPidProfileTunings(true, &high_kp, &high_ki, &high_kd);

    bool low_changed = false;
    bool high_changed = false;

    double legacy_kp = 0.0;
    double legacy_ki = 0.0;
    double legacy_kd = 0.0;
    const bool has_legacy_kp = get_number_like_pid(root, "kp", legacy_kp);
    const bool has_legacy_ki = get_number_like_pid(root, "ki", legacy_ki);
    const bool has_legacy_kd = get_number_like_pid(root, "kd", legacy_kd);
    if (has_legacy_kp || has_legacy_ki || has_legacy_kd) {
        if (!(has_legacy_kp && has_legacy_ki && has_legacy_kd)) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\":\"legacy_pid_requires_kp_ki_kd\"}");
            return ESP_OK;
        }
        low_kp = legacy_kp;
        low_ki = legacy_ki;
        low_kd = legacy_kd;
        high_kp = legacy_kp;
        high_ki = legacy_ki;
        high_kd = legacy_kd;
        low_changed = true;
        high_changed = true;
    }

    double value = 0.0;
    if (get_number_like_pid(root, "low_kp", value)) {
        low_kp = value;
        low_changed = true;
    }
    if (get_number_like_pid(root, "low_ki", value)) {
        low_ki = value;
        low_changed = true;
    }
    if (get_number_like_pid(root, "low_kd", value)) {
        low_kd = value;
        low_changed = true;
    }
    if (get_number_like_pid(root, "high_kp", value)) {
        high_kp = value;
        high_changed = true;
    }
    if (get_number_like_pid(root, "high_ki", value)) {
        high_ki = value;
        high_changed = true;
    }
    if (get_number_like_pid(root, "high_kd", value)) {
        high_kd = value;
        high_changed = true;
    }

    double offset_c = 0.0;
    bool offset_changed = false;
    if (get_number_like_pid(root, "temp_offset_c", offset_c) || get_number_like_pid(root, "offset", offset_c)) {
        offset_changed = true;
    }

    cJSON_Delete(root);

    if ((low_changed && (!std::isfinite(low_kp) || !std::isfinite(low_ki) || !std::isfinite(low_kd) ||
                         low_kp < 0.0 || low_ki < 0.0 || low_kd < 0.0)) ||
        (high_changed && (!std::isfinite(high_kp) || !std::isfinite(high_ki) || !std::isfinite(high_kd) ||
                          high_kp < 0.0 || high_ki < 0.0 || high_kd < 0.0))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_pid_values\"}");
        return ESP_OK;
    }
    if (offset_changed && (!std::isfinite(offset_c) || offset_c < -100.0 || offset_c > 100.0)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_temp_offset\"}");
        return ESP_OK;
    }

    bool ok = true;
    if (offset_changed) ok = thermalCtrl.setTemperatureOffset((float)offset_c) && ok;
    if (low_changed) ok = thermalCtrl.setPidProfileTunings(false, low_kp, low_ki, low_kd) && ok;
    if (high_changed) ok = thermalCtrl.setPidProfileTunings(true, high_kp, high_ki, high_kd) && ok;

    if (!ok) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"error\":\"update_failed\"}");
        return ESP_OK;
    }

    if (offset_changed || low_changed || high_changed) {
        wifiServer.notifySettingsChanged("pid_save");
    }

    return api_pid_get_handler(req);
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

    float target = thermalCtrl.getAutotuneTargetC();
    if (!body.empty()) {
        cJSON *root = cJSON_ParseWithLength(body.c_str(), body.size());
        if (root) {
            const cJSON *t = cJSON_GetObjectItem(root, "temp");
            if (cJSON_IsNumber(t)) target = (float)t->valuedouble;
            const cJSON *t2 = cJSON_GetObjectItem(root, "target");
            if (cJSON_IsNumber(t2)) target = (float)t2->valuedouble;
            const cJSON *t3 = cJSON_GetObjectItem(root, "targetC");
            if (cJSON_IsNumber(t3)) target = (float)t3->valuedouble;
            const cJSON *t4 = cJSON_GetObjectItem(root, "autotune_target_c");
            if (cJSON_IsNumber(t4)) target = (float)t4->valuedouble;
            cJSON_Delete(root);
        }
    }

    (void)thermalCtrl.setAutotuneTargetC(target);
    const bool ok = thermalCtrl.startAutotune(target);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    if (ok) {
        wifiServer.notifyAutotuneState("start");
        wifiServer.notifyCommandResult("autotune_start", {device_commands::ResultCode::Ok}, "started", "api");
        httpd_resp_sendstr(req, "{\"status\":\"started\"}");
    } else {
        const SafetyStats safety = thermalCtrl.getSafetyStats();
        const KilnState st = thermalCtrl.getState();
        const ThermalController::AutoTuneStatus tune = thermalCtrl.getAutotuneStatus();
        const bool sensor_ok = thermalCtrl.isSensorHealthy();
        const char *reason = "busy_or_fault";
        if (safety.faultActive) reason = "fault_active";
        else if (tune.active) reason = "autotune_active";
        else if (st.isFiring) reason = "already_firing";
        else if (!sensor_ok) reason = "sensor_unhealthy";
        device_commands::ResultCode rc = device_commands::ResultCode::InvalidPayload;
        if (!sensor_ok) rc = device_commands::ResultCode::SensorInvalid;
        wifiServer.notifyCommandResult("autotune_start", {rc}, reason, "api");
        char buf[128];
        std::snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", reason);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, buf);
    }
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_autotune_stop_handler(httpd_req_t *req) {
    (void)req;
    thermalCtrl.stopAutotune("Autotune stopped");
    wifiServer.notifyAutotuneState("stop");
    wifiServer.notifyCommandResult("autotune_stop", {device_commands::ResultCode::Ok}, "stopped", "api");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"stopped\"}");
    return ESP_OK;
}
