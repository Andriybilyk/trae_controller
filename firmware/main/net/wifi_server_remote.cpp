#include "net/wifi_server.h"

#include "net/remote_access.h"
#include "cJSON.h"

#include <cstdio>
#include <string>

esp_err_t WiFiServerManager::api_remote_get_handler(httpd_req_t *req) {
    remote_access_config_t cfg{};
    (void)remote_access_get_config(&cfg);

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "enabled", cfg.enabled);
    cJSON_AddStringToObject(doc, "uri", cfg.uri);
    cJSON_AddStringToObject(doc, "username", cfg.username);
    cJSON_AddBoolToObject(doc, "has_password", cfg.password[0] != '\0');
    cJSON_AddStringToObject(doc, "device_id", cfg.device_id);
    cJSON_AddBoolToObject(doc, "require_signed_commands", cfg.require_signed_commands);
    cJSON_AddBoolToObject(doc, "has_auth_key", cfg.auth_key[0] != '\0');
    cJSON_AddBoolToObject(doc, "has_ca_cert", remote_access_has_ca_pem());
    cJSON_AddBoolToObject(doc, "connected", remote_access_is_connected());

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

esp_err_t WiFiServerManager::api_remote_set_handler(httpd_req_t *req) {
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

    remote_access_config_t cfg{};
    (void)remote_access_get_config(&cfg);

    const cJSON *enabled = cJSON_GetObjectItem(incoming, "enabled");
    if (cJSON_IsBool(enabled)) cfg.enabled = cJSON_IsTrue(enabled);

    const cJSON *uri = cJSON_GetObjectItem(incoming, "uri");
    if (cJSON_IsString(uri) && uri->valuestring) {
        std::snprintf(cfg.uri, sizeof(cfg.uri), "%s", uri->valuestring);
    }

    const cJSON *username = cJSON_GetObjectItem(incoming, "username");
    if (cJSON_IsString(username) && username->valuestring) {
        std::snprintf(cfg.username, sizeof(cfg.username), "%s", username->valuestring);
    }

    const cJSON *password = cJSON_GetObjectItem(incoming, "password");
    if (cJSON_IsString(password) && password->valuestring) {
        std::snprintf(cfg.password, sizeof(cfg.password), "%s", password->valuestring);
    }

    const cJSON *clear_pass = cJSON_GetObjectItem(incoming, "clear_password");
    if (cJSON_IsBool(clear_pass) && cJSON_IsTrue(clear_pass)) {
        cfg.password[0] = '\0';
    }

    const cJSON *device_id = cJSON_GetObjectItem(incoming, "device_id");
    if (cJSON_IsString(device_id) && device_id->valuestring) {
        std::snprintf(cfg.device_id, sizeof(cfg.device_id), "%s", device_id->valuestring);
    }

    const cJSON *auth_key = cJSON_GetObjectItem(incoming, "auth_key");
    if (cJSON_IsString(auth_key) && auth_key->valuestring) {
        std::snprintf(cfg.auth_key, sizeof(cfg.auth_key), "%s", auth_key->valuestring);
    }

    const cJSON *clear_auth = cJSON_GetObjectItem(incoming, "clear_auth_key");
    if (cJSON_IsBool(clear_auth) && cJSON_IsTrue(clear_auth)) {
        cfg.auth_key[0] = '\0';
    }

    const cJSON *require_signed = cJSON_GetObjectItem(incoming, "require_signed_commands");
    if (cJSON_IsBool(require_signed)) {
        cfg.require_signed_commands = cJSON_IsTrue(require_signed);
    }

    bool clear_ca_cert = false;
    const cJSON *clear_ca = cJSON_GetObjectItem(incoming, "clear_ca_cert");
    if (cJSON_IsBool(clear_ca) && cJSON_IsTrue(clear_ca)) {
        clear_ca_cert = true;
    }

    std::string ca_pem;
    bool set_ca_cert = false;
    const cJSON *ca_cert = cJSON_GetObjectItem(incoming, "ca_pem");
    if (cJSON_IsString(ca_cert) && ca_cert->valuestring && ca_cert->valuestring[0]) {
        ca_pem = ca_cert->valuestring;
        set_ca_cert = true;
        clear_ca_cert = false;
    }

    cJSON_Delete(incoming);

    if (!remote_access_set_config(&cfg)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"save_failed\"}");
        return ESP_OK;
    }

    if (set_ca_cert && !remote_access_set_ca_pem(ca_pem.c_str())) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"ca_save_failed\"}");
        return ESP_OK;
    }

    if (clear_ca_cert && !remote_access_clear_ca_pem()) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"ca_clear_failed\"}");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}
