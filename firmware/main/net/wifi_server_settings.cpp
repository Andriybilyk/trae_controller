#include "net/wifi_server.h"

#include "app/device_commands.h"
#include "drivers/fan_driver.h"
#include "kiln_config/config_store.h"
#include "kiln_control/thermal_control.h"
#include "cJSON.h"

#include <cmath>
#include <cstdlib>
#include <string>

static bool get_number_like_local(const cJSON *obj, const char *key, double &out) {
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

esp_err_t WiFiServerManager::api_settings_get_handler(httpd_req_t *req) {
    cJSON *root = nullptr;
    const std::string existing = kiln_config_load_json_config();
    if (!existing.empty()) root = cJSON_Parse(existing.c_str());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        root = cJSON_CreateObject();
    }

    {
        const double v = (double)thermalCtrl.getTemperatureOffset();
        const double rounded = std::round(v * 10.0) / 10.0;
        cJSON_DeleteItemFromObject(root, "temp_offset_c");
        cJSON_AddNumberToObject(root, "temp_offset_c", rounded);
        cJSON_DeleteItemFromObject(root, "offset");
        cJSON_AddNumberToObject(root, "offset", rounded);
    }
    cJSON_DeleteItemFromObject(root, "fan_manual");
    cJSON_DeleteItemFromObject(root, "fan_auto");
    cJSON_DeleteItemFromObject(root, "fan_power");
    cJSON_DeleteItemFromObject(root, "fan_effective_power");
    cJSON_AddBoolToObject(root, "fan_manual", fan_driver_get_manual());
    cJSON_AddBoolToObject(root, "fan_auto", fan_driver_get_auto_enabled());
    cJSON_AddNumberToObject(root, "fan_power", (double)fan_driver_get_power_percent());
    cJSON_AddNumberToObject(root, "fan_effective_power", (double)fan_driver_get_effective_power_percent());
    float tmin = 0.0f, tmax = 0.0f;
    uint8_t pmin = 0, pmax = 0;
    fan_driver_get_auto_curve(&tmin, &tmax, &pmin, &pmax);
    cJSON_DeleteItemFromObject(root, "fan_temp_min_c");
    cJSON_DeleteItemFromObject(root, "fan_temp_max_c");
    cJSON_DeleteItemFromObject(root, "fan_power_min");
    cJSON_DeleteItemFromObject(root, "fan_power_max");
    cJSON_AddNumberToObject(root, "fan_temp_min_c", (double)tmin);
    cJSON_AddNumberToObject(root, "fan_temp_max_c", (double)tmax);
    cJSON_AddNumberToObject(root, "fan_power_min", (double)pmin);
    cJSON_AddNumberToObject(root, "fan_power_max", (double)pmax);
    cJSON *maxC = cJSON_GetObjectItem(root, "maxC");
    if (!cJSON_IsNumber(maxC)) {
        cJSON_DeleteItemFromObject(root, "maxC");
        cJSON_AddNumberToObject(root, "maxC", 1300.0);
    }

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

    cJSON *root = nullptr;
    const std::string existing = kiln_config_load_json_config();
    if (!existing.empty()) root = cJSON_Parse(existing.c_str());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        root = cJSON_CreateObject();
    }

    {
        double offset_value = 0.0;
        if (get_number_like_local(incoming, "offset", offset_value) || get_number_like_local(incoming, "temp_offset_c", offset_value)) {
            if (offset_value >= -100.0 && offset_value <= 100.0) {
                (void)thermalCtrl.setTemperatureOffset((float)offset_value);
            }
        }
    }
    {
        double v = 0.0;
        if (get_number_like_local(incoming, "ssrCycles", v)) {
            cJSON_DeleteItemFromObject(root, "ssrCycles");
            cJSON_AddNumberToObject(root, "ssrCycles", v);
        }
    }
    {
        double v = 0.0;
        if (get_number_like_local(incoming, "wattage", v)) {
            cJSON_DeleteItemFromObject(root, "wattage");
            cJSON_AddNumberToObject(root, "wattage", v);
        }
    }
    {
        double v = 0.0;
        if (get_number_like_local(incoming, "costPerKwh", v)) {
            cJSON_DeleteItemFromObject(root, "costPerKwh");
            cJSON_AddNumberToObject(root, "costPerKwh", v);
        }
    }

    const cJSON *currency = cJSON_GetObjectItem(incoming, "currency");
    if (cJSON_IsString(currency) && currency->valuestring) {
        cJSON_DeleteItemFromObject(root, "currency");
        cJSON_AddStringToObject(root, "currency", currency->valuestring);
    }

    {
        double v = 0.0;
        if (get_number_like_local(incoming, "zones", v)) {
            cJSON_DeleteItemFromObject(root, "zones");
            cJSON_AddNumberToObject(root, "zones", v);
        }
    }

    {
        double v = 0.0;
        if (get_number_like_local(incoming, "maxC", v)) {
            if (v < 100.0) v = 100.0;
            if (v > 1300.0) v = 1300.0;
            cJSON_DeleteItemFromObject(root, "maxC");
            cJSON_AddNumberToObject(root, "maxC", v);
            (void)thermalCtrl.setUserMaxTemperatureC((float)v);
        }
    }

    device_commands::FanConfig fan_cfg = device_commands::current_fan_config();
    const cJSON *fanManual = cJSON_GetObjectItem(incoming, "fan_manual");
    if (cJSON_IsBool(fanManual)) fan_cfg.manual = cJSON_IsTrue(fanManual);
    const cJSON *fanAuto = cJSON_GetObjectItem(incoming, "fan_auto");
    if (cJSON_IsBool(fanAuto)) fan_cfg.auto_enabled = cJSON_IsTrue(fanAuto);
    {
        double dv = 0.0;
        if (get_number_like_local(incoming, "fan_power", dv)) {
            int v = (int)dv;
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            fan_cfg.power = static_cast<uint8_t>(v);
        }
    }
    {
        double v = 0.0;
        if (get_number_like_local(incoming, "fan_temp_min_c", v)) fan_cfg.temp_min_c = (float)v;
        if (get_number_like_local(incoming, "fan_temp_max_c", v)) fan_cfg.temp_max_c = (float)v;
    }
    {
        double dv = 0.0;
        if (get_number_like_local(incoming, "fan_power_min", dv)) {
            int v = (int)dv;
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            fan_cfg.power_min = static_cast<uint8_t>(v);
        }
    }
    {
        double dv = 0.0;
        if (get_number_like_local(incoming, "fan_power_max", dv)) {
            int v = (int)dv;
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            fan_cfg.power_max = static_cast<uint8_t>(v);
        }
    }

    device_commands::apply_fan_config(fan_cfg);
    fan_cfg = device_commands::current_fan_config();
    cJSON_DeleteItemFromObject(root, "fan_manual");
    cJSON_DeleteItemFromObject(root, "fan_auto");
    cJSON_DeleteItemFromObject(root, "fan_power");
    cJSON_DeleteItemFromObject(root, "fan_temp_min_c");
    cJSON_DeleteItemFromObject(root, "fan_temp_max_c");
    cJSON_DeleteItemFromObject(root, "fan_power_min");
    cJSON_DeleteItemFromObject(root, "fan_power_max");
    cJSON_AddBoolToObject(root, "fan_manual", fan_cfg.manual);
    cJSON_AddBoolToObject(root, "fan_auto", fan_cfg.auto_enabled);
    cJSON_AddNumberToObject(root, "fan_power", (double)fan_cfg.power);
    cJSON_AddNumberToObject(root, "fan_temp_min_c", (double)fan_cfg.temp_min_c);
    cJSON_AddNumberToObject(root, "fan_temp_max_c", (double)fan_cfg.temp_max_c);
    cJSON_AddNumberToObject(root, "fan_power_min", (double)fan_cfg.power_min);
    cJSON_AddNumberToObject(root, "fan_power_max", (double)fan_cfg.power_max);

    cJSON_DeleteItemFromObject(root, "temp_offset_c");
    cJSON_AddNumberToObject(root, "temp_offset_c", (double)thermalCtrl.getTemperatureOffset());

    char *rendered = cJSON_PrintUnformatted(root);
    const std::string out = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(root);
    cJSON_Delete(incoming);

    if (!kiln_config_save_json_config(out)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    wifiServer.notifySettingsChanged("save");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}
