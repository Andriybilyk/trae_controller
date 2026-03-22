#include "net/wifi_server.h"

#include "app/device_commands.h"
#include "drivers/fan_driver.h"
#include "cJSON.h"

#include <string>

esp_err_t WiFiServerManager::api_fan_get_handler(httpd_req_t *req) {
    float tmin = 0.0f, tmax = 0.0f;
    uint8_t pmin = 0, pmax = 0;
    fan_driver_get_auto_curve(&tmin, &tmax, &pmin, &pmax);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "manual", fan_driver_get_manual());
    cJSON_AddBoolToObject(doc, "auto", fan_driver_get_auto_enabled());
    cJSON_AddNumberToObject(doc, "power", (double)fan_driver_get_power_percent());
    cJSON_AddNumberToObject(doc, "effective_power", (double)fan_driver_get_effective_power_percent());
    cJSON_AddNumberToObject(doc, "temp_min_c", (double)tmin);
    cJSON_AddNumberToObject(doc, "temp_max_c", (double)tmax);
    cJSON_AddNumberToObject(doc, "power_min", (double)pmin);
    cJSON_AddNumberToObject(doc, "power_max", (double)pmax);

    char *rendered = cJSON_PrintUnformatted(doc);
    std::string output = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_fan_set_handler(httpd_req_t *req) {
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

    device_commands::FanConfig cfg = device_commands::current_fan_config();

    const cJSON *manual_item = cJSON_GetObjectItem(root, "manual");
    if (cJSON_IsBool(manual_item)) cfg.manual = cJSON_IsTrue(manual_item);

    const cJSON *auto_item = cJSON_GetObjectItem(root, "auto");
    if (cJSON_IsBool(auto_item)) cfg.auto_enabled = cJSON_IsTrue(auto_item);

    const cJSON *power_item = cJSON_GetObjectItem(root, "power");
    if (cJSON_IsNumber(power_item)) {
        int v = (int)power_item->valuedouble;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        cfg.power = (uint8_t)v;
    }

    const cJSON *tmin_item = cJSON_GetObjectItem(root, "temp_min_c");
    if (cJSON_IsNumber(tmin_item)) cfg.temp_min_c = (float)tmin_item->valuedouble;
    const cJSON *tmax_item = cJSON_GetObjectItem(root, "temp_max_c");
    if (cJSON_IsNumber(tmax_item)) cfg.temp_max_c = (float)tmax_item->valuedouble;
    const cJSON *pmin_item = cJSON_GetObjectItem(root, "power_min");
    if (cJSON_IsNumber(pmin_item)) {
        int v = (int)pmin_item->valuedouble;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        cfg.power_min = (uint8_t)v;
    }
    const cJSON *pmax_item = cJSON_GetObjectItem(root, "power_max");
    if (cJSON_IsNumber(pmax_item)) {
        int v = (int)pmax_item->valuedouble;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        cfg.power_max = (uint8_t)v;
    }

    device_commands::apply_fan_config(cfg);

    wifiServer.notifySettingsChanged("fan");

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}
