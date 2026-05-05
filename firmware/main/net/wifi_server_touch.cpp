#include "net/wifi_server.h"

#include "config/board_profile.h"
#include "drivers/display_driver.h"
#include "cJSON.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cmath>
#include <string>

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

esp_err_t WiFiServerManager::api_touch_affine_get_handler(httpd_req_t *req) {
    bool enabled = false;
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 0.0f, e = 1.0f, f = 0.0f;
    display_driver_get_touch_affine(&enabled, &a, &b, &c, &d, &e, &f);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "enabled", enabled);
    cJSON_AddNumberToObject(doc, "a", (double)a);
    cJSON_AddNumberToObject(doc, "b", (double)b);
    cJSON_AddNumberToObject(doc, "c", (double)c);
    cJSON_AddNumberToObject(doc, "d", (double)d);
    cJSON_AddNumberToObject(doc, "e", (double)e);
    cJSON_AddNumberToObject(doc, "f", (double)f);

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

esp_err_t WiFiServerManager::api_touch_affine_set_handler(httpd_req_t *req) {
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
        (void)display_driver_set_touch_affine(false, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Pragma", "no-cache");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"reset\":true}");
        return ESP_OK;
    }

    bool enabled = false;
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 0.0f, e = 1.0f, f = 0.0f;
    display_driver_get_touch_affine(&enabled, &a, &b, &c, &d, &e, &f);

    const cJSON *en = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(en)) enabled = cJSON_IsTrue(en);

    const cJSON *ja = cJSON_GetObjectItem(root, "a");
    const cJSON *jb = cJSON_GetObjectItem(root, "b");
    const cJSON *jc = cJSON_GetObjectItem(root, "c");
    const cJSON *jd = cJSON_GetObjectItem(root, "d");
    const cJSON *je = cJSON_GetObjectItem(root, "e");
    const cJSON *jf = cJSON_GetObjectItem(root, "f");
    if (cJSON_IsNumber(ja)) a = (float)ja->valuedouble;
    if (cJSON_IsNumber(jb)) b = (float)jb->valuedouble;
    if (cJSON_IsNumber(jc)) c = (float)jc->valuedouble;
    if (cJSON_IsNumber(jd)) d = (float)jd->valuedouble;
    if (cJSON_IsNumber(je)) e = (float)je->valuedouble;
    if (cJSON_IsNumber(jf)) f = (float)jf->valuedouble;

    cJSON_Delete(root);

    if (!display_driver_set_touch_affine(enabled, a, b, c, d, e, f)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_touch_grid_get_handler(httpd_req_t *req) {
    bool enabled = false;
    float dx[9]{};
    float dy[9]{};
    display_driver_get_touch_grid(&enabled, dx, dy);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "enabled", enabled);
    cJSON *dx_arr = cJSON_AddArrayToObject(doc, "dx");
    cJSON *dy_arr = cJSON_AddArrayToObject(doc, "dy");
    for (int i = 0; i < 9; ++i) {
        cJSON_AddItemToArray(dx_arr, cJSON_CreateNumber((double)dx[i]));
        cJSON_AddItemToArray(dy_arr, cJSON_CreateNumber((double)dy[i]));
    }

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

static bool parse_float_array_9(const cJSON *arr, float out[9]) {
    if (!arr || !cJSON_IsArray(arr)) return false;
    if (cJSON_GetArraySize(arr) != 9) return false;
    for (int i = 0; i < 9; ++i) {
        const cJSON *it = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsNumber(it)) return false;
        const double v = it->valuedouble;
        if (!std::isfinite(v)) return false;
        out[i] = (float)v;
    }
    return true;
}

esp_err_t WiFiServerManager::api_touch_grid_set_handler(httpd_req_t *req) {
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
        (void)display_driver_set_touch_grid(false, nullptr, nullptr);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Pragma", "no-cache");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"reset\":true}");
        return ESP_OK;
    }

    bool enabled = false;
    const cJSON *en = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(en)) enabled = cJSON_IsTrue(en);

    float dx[9]{};
    float dy[9]{};
    if (enabled) {
        const cJSON *dx_arr = cJSON_GetObjectItem(root, "dx");
        const cJSON *dy_arr = cJSON_GetObjectItem(root, "dy");
        if (!parse_float_array_9(dx_arr, dx) || !parse_float_array_9(dy_arr, dy)) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\":\"invalid_params\"}");
            return ESP_OK;
        }
    }

    cJSON_Delete(root);

    if (!display_driver_set_touch_grid(enabled, dx, dy)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_touch_profile_get_handler(httpd_req_t *req) {
    int mode = 0;
    int hz = 0;
    display_driver_get_touch_spi(&mode, &hz);

    bool swap_xy = false;
    bool mirror_x = false;
    bool mirror_y = false;
    display_driver_get_touch_transform(&swap_xy, &mirror_x, &mirror_y);

    bool cal_enabled = false;
    uint16_t cal_left = 0;
    uint16_t cal_right = 0;
    uint16_t cal_top = 0;
    uint16_t cal_bottom = 0;
    display_driver_get_touch_calibration(&cal_enabled, &cal_left, &cal_right, &cal_top, &cal_bottom);

    bool aff_enabled = false;
    float a = 1.0f;
    float b = 0.0f;
    float c = 0.0f;
    float d = 0.0f;
    float e = 1.0f;
    float f = 0.0f;
    display_driver_get_touch_affine(&aff_enabled, &a, &b, &c, &d, &e, &f);

    bool grid_enabled = false;
    float dx[9]{};
    float dy[9]{};
    display_driver_get_touch_grid(&grid_enabled, dx, dy);

    int sclk = 0;
    int mosi = 0;
    int miso = 0;
    display_driver_get_touch_pins(&sclk, &mosi, &miso);

    cJSON *doc = cJSON_CreateObject();

    cJSON *spi = cJSON_AddObjectToObject(doc, "spi");
    cJSON_AddNumberToObject(spi, "mode", mode);
    cJSON_AddNumberToObject(spi, "hz", hz);

    cJSON *tr = cJSON_AddObjectToObject(doc, "transform");
    cJSON_AddBoolToObject(tr, "swap_xy", swap_xy);
    cJSON_AddBoolToObject(tr, "mirror_x", mirror_x);
    cJSON_AddBoolToObject(tr, "mirror_y", mirror_y);

    cJSON *cal = cJSON_AddObjectToObject(doc, "calibration");
    cJSON_AddBoolToObject(cal, "enabled", cal_enabled);
    cJSON_AddNumberToObject(cal, "left", (int)cal_left);
    cJSON_AddNumberToObject(cal, "right", (int)cal_right);
    cJSON_AddNumberToObject(cal, "top", (int)cal_top);
    cJSON_AddNumberToObject(cal, "bottom", (int)cal_bottom);

    cJSON *aff = cJSON_AddObjectToObject(doc, "affine");
    cJSON_AddBoolToObject(aff, "enabled", aff_enabled);
    cJSON_AddNumberToObject(aff, "a", (double)a);
    cJSON_AddNumberToObject(aff, "b", (double)b);
    cJSON_AddNumberToObject(aff, "c", (double)c);
    cJSON_AddNumberToObject(aff, "d", (double)d);
    cJSON_AddNumberToObject(aff, "e", (double)e);
    cJSON_AddNumberToObject(aff, "f", (double)f);

    cJSON *grid = cJSON_AddObjectToObject(doc, "grid");
    cJSON_AddBoolToObject(grid, "enabled", grid_enabled);
    cJSON *dx_arr = cJSON_AddArrayToObject(grid, "dx");
    cJSON *dy_arr = cJSON_AddArrayToObject(grid, "dy");
    for (int i = 0; i < 9; ++i) {
        cJSON_AddItemToArray(dx_arr, cJSON_CreateNumber((double)dx[i]));
        cJSON_AddItemToArray(dy_arr, cJSON_CreateNumber((double)dy[i]));
    }

    cJSON *pins = cJSON_AddObjectToObject(doc, "pins");
    cJSON_AddNumberToObject(pins, "sclk", sclk);
    cJSON_AddNumberToObject(pins, "mosi", mosi);
    cJSON_AddNumberToObject(pins, "miso", miso);

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

esp_err_t WiFiServerManager::api_touch_profile_set_handler(httpd_req_t *req) {
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

    bool need_reboot = false;

    const cJSON *spi = cJSON_GetObjectItem(root, "spi");
    if (spi && cJSON_IsObject(spi)) {
        int mode = 0;
        int hz = 0;
        display_driver_get_touch_spi(&mode, &hz);
        const cJSON *jm = cJSON_GetObjectItem(spi, "mode");
        const cJSON *jhz = cJSON_GetObjectItem(spi, "hz");
        if (cJSON_IsNumber(jm)) mode = (int)jm->valuedouble;
        if (cJSON_IsNumber(jhz)) hz = (int)jhz->valuedouble;
        if (!display_driver_set_touch_spi(mode, hz)) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\":\"invalid_spi\"}");
            return ESP_OK;
        }
    }

    const cJSON *tr = cJSON_GetObjectItem(root, "transform");
    if (tr && cJSON_IsObject(tr)) {
        bool swap_xy = false;
        bool mirror_x = false;
        bool mirror_y = false;
        display_driver_get_touch_transform(&swap_xy, &mirror_x, &mirror_y);
        const cJSON *sx = cJSON_GetObjectItem(tr, "swap_xy");
        const cJSON *mx = cJSON_GetObjectItem(tr, "mirror_x");
        const cJSON *my = cJSON_GetObjectItem(tr, "mirror_y");
        if (cJSON_IsBool(sx)) swap_xy = cJSON_IsTrue(sx);
        if (cJSON_IsBool(mx)) mirror_x = cJSON_IsTrue(mx);
        if (cJSON_IsBool(my)) mirror_y = cJSON_IsTrue(my);
        if (!display_driver_set_touch_transform(swap_xy, mirror_x, mirror_y)) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\":\"invalid_transform\"}");
            return ESP_OK;
        }
    }

    const cJSON *cal = cJSON_GetObjectItem(root, "calibration");
    if (cal && cJSON_IsObject(cal)) {
        const cJSON *reset = cJSON_GetObjectItem(cal, "reset");
        if (cJSON_IsBool(reset) && cJSON_IsTrue(reset)) {
            display_driver_reset_touch_calibration();
        } else {
            bool enabled = false;
            uint16_t left = 0;
            uint16_t right = 0;
            uint16_t top = 0;
            uint16_t bottom = 0;
            display_driver_get_touch_calibration(&enabled, &left, &right, &top, &bottom);
            const cJSON *en = cJSON_GetObjectItem(cal, "enabled");
            const cJSON *jl = cJSON_GetObjectItem(cal, "left");
            const cJSON *jr = cJSON_GetObjectItem(cal, "right");
            const cJSON *jt = cJSON_GetObjectItem(cal, "top");
            const cJSON *jb = cJSON_GetObjectItem(cal, "bottom");
            if (cJSON_IsBool(en)) enabled = cJSON_IsTrue(en);
            if (cJSON_IsNumber(jl)) left = (uint16_t)jl->valuedouble;
            if (cJSON_IsNumber(jr)) right = (uint16_t)jr->valuedouble;
            if (cJSON_IsNumber(jt)) top = (uint16_t)jt->valuedouble;
            if (cJSON_IsNumber(jb)) bottom = (uint16_t)jb->valuedouble;
            if (!display_driver_set_touch_calibration(enabled, left, right, top, bottom)) {
                cJSON_Delete(root);
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_sendstr(req, "{\"error\":\"invalid_calibration\"}");
                return ESP_OK;
            }
        }
    }

    const cJSON *aff = cJSON_GetObjectItem(root, "affine");
    if (aff && cJSON_IsObject(aff)) {
        const cJSON *reset = cJSON_GetObjectItem(aff, "reset");
        if (cJSON_IsBool(reset) && cJSON_IsTrue(reset)) {
            (void)display_driver_set_touch_affine(false, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
        } else {
            bool enabled = false;
            float a = 1.0f;
            float b = 0.0f;
            float c = 0.0f;
            float d = 0.0f;
            float e = 1.0f;
            float f = 0.0f;
            display_driver_get_touch_affine(&enabled, &a, &b, &c, &d, &e, &f);
            const cJSON *en = cJSON_GetObjectItem(aff, "enabled");
            const cJSON *ja = cJSON_GetObjectItem(aff, "a");
            const cJSON *jb = cJSON_GetObjectItem(aff, "b");
            const cJSON *jc = cJSON_GetObjectItem(aff, "c");
            const cJSON *jd = cJSON_GetObjectItem(aff, "d");
            const cJSON *je = cJSON_GetObjectItem(aff, "e");
            const cJSON *jf = cJSON_GetObjectItem(aff, "f");
            if (cJSON_IsBool(en)) enabled = cJSON_IsTrue(en);
            if (cJSON_IsNumber(ja)) a = (float)ja->valuedouble;
            if (cJSON_IsNumber(jb)) b = (float)jb->valuedouble;
            if (cJSON_IsNumber(jc)) c = (float)jc->valuedouble;
            if (cJSON_IsNumber(jd)) d = (float)jd->valuedouble;
            if (cJSON_IsNumber(je)) e = (float)je->valuedouble;
            if (cJSON_IsNumber(jf)) f = (float)jf->valuedouble;
            if (!display_driver_set_touch_affine(enabled, a, b, c, d, e, f)) {
                cJSON_Delete(root);
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_sendstr(req, "{\"error\":\"invalid_affine\"}");
                return ESP_OK;
            }
        }
    }

    const cJSON *grid = cJSON_GetObjectItem(root, "grid");
    if (grid && cJSON_IsObject(grid)) {
        const cJSON *reset = cJSON_GetObjectItem(grid, "reset");
        if (cJSON_IsBool(reset) && cJSON_IsTrue(reset)) {
            (void)display_driver_set_touch_grid(false, nullptr, nullptr);
        } else {
            bool enabled = false;
            const cJSON *en = cJSON_GetObjectItem(grid, "enabled");
            if (cJSON_IsBool(en)) enabled = cJSON_IsTrue(en);
            float dx[9]{};
            float dy[9]{};
            if (enabled) {
                const cJSON *dx_arr = cJSON_GetObjectItem(grid, "dx");
                const cJSON *dy_arr = cJSON_GetObjectItem(grid, "dy");
                if (!parse_float_array_9(dx_arr, dx) || !parse_float_array_9(dy_arr, dy)) {
                    cJSON_Delete(root);
                    httpd_resp_set_status(req, "400 Bad Request");
                    httpd_resp_sendstr(req, "{\"error\":\"invalid_grid\"}");
                    return ESP_OK;
                }
            }
            if (!display_driver_set_touch_grid(enabled, dx, dy)) {
                cJSON_Delete(root);
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_sendstr(req, "{\"error\":\"invalid_grid\"}");
                return ESP_OK;
            }
        }
    }

    const cJSON *pins = cJSON_GetObjectItem(root, "pins");
    if (pins && cJSON_IsObject(pins)) {
        int sclk = 0;
        int mosi = 0;
        int miso = 0;
        display_driver_get_touch_pins(&sclk, &mosi, &miso);
        const cJSON *jsclk = cJSON_GetObjectItem(pins, "sclk");
        const cJSON *jmosi = cJSON_GetObjectItem(pins, "mosi");
        const cJSON *jmiso = cJSON_GetObjectItem(pins, "miso");
        if (cJSON_IsNumber(jsclk)) sclk = (int)jsclk->valuedouble;
        if (cJSON_IsNumber(jmosi)) mosi = (int)jmosi->valuedouble;
        if (cJSON_IsNumber(jmiso)) miso = (int)jmiso->valuedouble;
        if (!display_driver_set_touch_pins(sclk, mosi, miso)) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\":\"invalid_pins\"}");
            return ESP_OK;
        }
        need_reboot = true;
    }

    cJSON_Delete(root);

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    if (need_reboot) {
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"rebooting\":true}");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
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
    bool pressed = false;
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        display_driver_get_touch_debug(&rx, &ry, &z1, &pressed);
    } else {
        pressed = display_driver_touch_probe(&rx, &ry, &z1);
    }

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
    bool pressed = false;
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        display_driver_get_touch_debug(&rx, &ry, &z1, &pressed);
    } else {
        pressed = display_driver_touch_probe_raw(&rx, &ry, &z1);
    }

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
