#include "net/wifi_server.h"

#include "kiln_config/fs_utils.h"
#include "kiln_control/thermal_control.h"
#include "cJSON.h"

#include <dirent.h>
#include <string>
#include <unistd.h>

static std::string uri_suffix_after(const char *uri, const char *prefix) {
    if (!uri || !prefix) return {};
    const std::string up(uri);
    const std::string pp(prefix);
    if (up.size() <= pp.size()) return {};
    if (up.rfind(pp, 0) != 0) return {};
    if (up[pp.size()] != '/') return {};
    return up.substr(pp.size() + 1);
}

static int clear_history_files() {
    int removed = 0;

    const std::string list = kiln_fs_read_text("/littlefs/history.json");
    if (!list.empty()) {
        cJSON *root = cJSON_Parse(list.c_str());
        if (cJSON_IsArray(root)) {
            const int n = cJSON_GetArraySize(root);
            for (int i = 0; i < n; ++i) {
                cJSON *item = cJSON_GetArrayItem(root, i);
                const cJSON *id = cJSON_GetObjectItem(item, "id");
                if (!cJSON_IsString(id) || !id->valuestring || !id->valuestring[0]) continue;
                std::string path = "/littlefs/history_";
                path += id->valuestring;
                path += ".json";
                if (unlink(path.c_str()) == 0) removed++;
            }
        }
        if (root) cJSON_Delete(root);
    }

    if (DIR *dir = opendir("/littlefs")) {
        while (struct dirent *entry = readdir(dir)) {
            const std::string name(entry->d_name);
            if (name.rfind("history_", 0) != 0) continue;
            if (name.size() < 14 || name.substr(name.size() - 5) != ".json") continue;
            std::string path = "/littlefs/";
            path += name;
            if (unlink(path.c_str()) == 0) removed++;
        }
        closedir(dir);
    }

    if (!kiln_fs_write_text_atomic("/littlefs/history.json", "[]")) return -1;
    return removed;
}

esp_err_t WiFiServerManager::api_history_list_handler(httpd_req_t *req) {
    const std::string file = kiln_fs_read_text("/littlefs/history.json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, file.empty() ? "[]" : file.c_str());
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_history_clear_handler(httpd_req_t *req) {
    const KilnState st = thermalCtrl.getState();
    if (st.isFiring) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"busy_firing\",\"message\":\"Cannot clear history while firing\"}");
        return ESP_OK;
    }

    const int removed = clear_history_files();
    if (removed < 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"code\":\"history_clear_failed\",\"message\":\"Failed to clear history\"}");
        return ESP_OK;
    }

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "ok", true);
    cJSON_AddNumberToObject(doc, "removed", removed);
    char *rendered = cJSON_PrintUnformatted(doc);
    std::string output = rendered ? rendered : "{\"ok\":true}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
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

    const std::string file = kiln_fs_read_text(path.c_str());
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
