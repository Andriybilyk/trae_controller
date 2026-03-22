#include "net/wifi_server.h"

#include "cJSON.h"

#include <cmath>
#include <string>

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
