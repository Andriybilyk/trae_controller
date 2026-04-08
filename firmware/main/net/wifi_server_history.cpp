#include "net/wifi_server.h"

#include "kiln_config/fs_utils.h"
#include "kiln_control/thermal_control.h"
#include "cJSON.h"

#include <dirent.h>
#include <string>
#include <unistd.h>

static std::string format_duration_label(int minutes, bool is_ua) {
    const int safe_minutes = minutes < 0 ? 0 : minutes;
    const int hours = safe_minutes / 60;
    const int mins = safe_minutes % 60;
    if (hours > 0) {
        if (is_ua) {
            return std::to_string(hours) + " год " + std::to_string(mins) + " хв";
        }
        return std::to_string(hours) + "h " + std::to_string(mins) + "m";
    }
    if (is_ua) {
        return std::to_string(std::max(1, mins)) + " хв";
    }
    return std::to_string(std::max(1, mins)) + " min";
}

static std::string to_upper_ascii(std::string s) {
    for (char &c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

static void replace_item(cJSON *obj, const char *key, cJSON *item) {
    if (!obj || !key) return;
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    cJSON_AddItemToObject(obj, key, item);
}

static int get_step_int(const cJSON *step, const char *key, int fallback = 0) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(step, key);
    return cJSON_IsNumber(v) ? v->valueint : fallback;
}

static int get_step_hold_minutes(const cJSON *step) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(step, "holdTime");
    if (cJSON_IsNumber(v)) return v->valueint;
    v = cJSON_GetObjectItemCaseSensitive(step, "hold");
    if (cJSON_IsNumber(v)) return v->valueint;
    v = cJSON_GetObjectItemCaseSensitive(step, "time");
    if (cJSON_IsNumber(v)) return v->valueint;
    v = cJSON_GetObjectItemCaseSensitive(step, "duration");
    if (cJSON_IsNumber(v)) return v->valueint;
    return 0;
}

static cJSON *build_planned_from_schedule_name(const std::string &schedule_name, double start_temp) {
    if (schedule_name.empty()) return nullptr;
    const std::string schedules_raw = kiln_fs_read_text("/littlefs/schedules.json");
    if (schedules_raw.empty()) return nullptr;
    cJSON *root = cJSON_Parse(schedules_raw.c_str());
    if (!cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return nullptr;
    }

    cJSON *match = nullptr;
    const int n = cJSON_GetArraySize(root);
    for (int i = 0; i < n; ++i) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (cJSON_IsString(name) && name->valuestring && schedule_name == name->valuestring) {
            match = item;
            break;
        }
    }
    if (!match) {
        cJSON_Delete(root);
        return nullptr;
    }

    cJSON *steps = cJSON_GetObjectItemCaseSensitive(match, "steps");
    if (!cJSON_IsArray(steps)) steps = cJSON_GetObjectItemCaseSensitive(match, "segments");
    if (!cJSON_IsArray(steps)) {
        cJSON_Delete(root);
        return nullptr;
    }

    cJSON *planned = cJSON_CreateArray();
    double t = 0.0;
    double current = start_temp;
    cJSON *p0 = cJSON_CreateObject();
    cJSON_AddNumberToObject(p0, "t", t);
    cJSON_AddNumberToObject(p0, "temp", current);
    cJSON_AddItemToArray(planned, p0);

    const int steps_n = cJSON_GetArraySize(steps);
    for (int i = 0; i < steps_n; ++i) {
        const cJSON *step = cJSON_GetArrayItem(steps, i);
        if (!cJSON_IsObject(step)) continue;
        const int rate = std::max(0, get_step_int(step, "rate", 0));
        const int target = get_step_int(step, "target", static_cast<int>(current));
        const int hold_min = std::max(0, get_step_hold_minutes(step));

        if (rate > 0) {
            const double delta = std::abs(static_cast<double>(target) - current);
            const double ramp_min = (delta / static_cast<double>(rate)) * 60.0;
            t += ramp_min;
        }
        current = static_cast<double>(target);
        cJSON *p1 = cJSON_CreateObject();
        cJSON_AddNumberToObject(p1, "t", t);
        cJSON_AddNumberToObject(p1, "temp", current);
        cJSON_AddItemToArray(planned, p1);

        if (hold_min > 0) {
            t += hold_min;
            cJSON *p2 = cJSON_CreateObject();
            cJSON_AddNumberToObject(p2, "t", t);
            cJSON_AddNumberToObject(p2, "temp", current);
            cJSON_AddItemToArray(planned, p2);
        }
    }

    cJSON_Delete(root);
    return planned;
}

static std::string build_history_list_enriched(const std::string &raw) {
    cJSON *root = cJSON_Parse(raw.c_str());
    if (!cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return raw;
    }
    cJSON *out = cJSON_CreateArray();
    const int n = cJSON_GetArraySize(root);
    for (int i = 0; i < n; ++i) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        cJSON *obj = cJSON_Duplicate(item, true);
        if (!cJSON_IsObject(obj)) {
            if (obj) cJSON_Delete(obj);
            obj = cJSON_CreateObject();
        }
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(obj, "name");
        if (!cJSON_IsString(name)) name = cJSON_GetObjectItemCaseSensitive(obj, "scheduleName");
        if (!cJSON_IsString(name)) name = cJSON_GetObjectItemCaseSensitive(obj, "program");
        const std::string title = (cJSON_IsString(name) && name->valuestring && name->valuestring[0])
            ? name->valuestring
            : "Untitled";
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(obj, "status");
        if (!cJSON_IsString(status)) status = cJSON_GetObjectItemCaseSensitive(obj, "result");
        const std::string status_raw = (cJSON_IsString(status) && status->valuestring) ? status->valuestring : "";
        const std::string status_code = to_upper_ascii(status_raw);
        const int duration = cJSON_GetObjectItemCaseSensitive(obj, "duration")
                                 ? cJSON_GetObjectItemCaseSensitive(obj, "duration")->valueint
                                 : 0;
        const double peak_temp = cJSON_GetObjectItemCaseSensitive(obj, "peakTemp")
                                     ? cJSON_GetObjectItemCaseSensitive(obj, "peakTemp")->valuedouble
                                     : 0.0;
        const int total_steps = cJSON_GetObjectItemCaseSensitive(obj, "totalSteps")
                                    ? cJSON_GetObjectItemCaseSensitive(obj, "totalSteps")->valueint
                                    : 0;
        const int completed_steps = cJSON_GetObjectItemCaseSensitive(obj, "completedSteps")
                                        ? cJSON_GetObjectItemCaseSensitive(obj, "completedSteps")->valueint
                                        : 0;

        std::string subtitle_en = format_duration_label(duration, false);
        std::string subtitle_ua = format_duration_label(duration, true);
        if (peak_temp > 0.0) {
            subtitle_en += " • peak " + std::to_string(static_cast<int>(peak_temp + 0.5)) + "°C";
            subtitle_ua += " • пік " + std::to_string(static_cast<int>(peak_temp + 0.5)) + "°C";
        }
        if (total_steps > 0) {
            subtitle_en += " • steps " + std::to_string(std::max(0, completed_steps)) + "/" + std::to_string(total_steps);
            subtitle_ua += " • кроки " + std::to_string(std::max(0, completed_steps)) + "/" + std::to_string(total_steps);
        }

        replace_item(obj, "title", cJSON_CreateString(title.c_str()));
        replace_item(obj, "statusCode", cJSON_CreateString(status_code.c_str()));
        replace_item(obj, "subtitle_en", cJSON_CreateString(subtitle_en.c_str()));
        replace_item(obj, "subtitle_ua", cJSON_CreateString(subtitle_ua.c_str()));
        cJSON_AddItemToArray(out, obj);
    }
    char *rendered = cJSON_PrintUnformatted(out);
    std::string output = rendered ? rendered : raw;
    if (rendered) free(rendered);
    cJSON_Delete(out);
    cJSON_Delete(root);
    return output;
}

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
    const std::string payload = file.empty() ? "[]" : build_history_list_enriched(file);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, payload.c_str());
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

    cJSON *root = cJSON_Parse(file.c_str());
    if (cJSON_IsObject(root)) {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsArray(data) && cJSON_GetArraySize(data) > 0) {
            const cJSON *first = cJSON_GetArrayItem(data, 0);
            const cJSON *first_ts_item = first ? cJSON_GetObjectItemCaseSensitive(first, "timestamp") : nullptr;
            const double first_ts = first_ts_item ? first_ts_item->valuedouble : 0.0;
            cJSON *planned = cJSON_CreateArray();
            cJSON *actual = cJSON_CreateArray();
            const int n = cJSON_GetArraySize(data);
            int last_min = 0;
            for (int i = 0; i < n; ++i) {
                const cJSON *row = cJSON_GetArrayItem(data, i);
                const cJSON *ts = cJSON_GetObjectItemCaseSensitive(row, "timestamp");
                const cJSON *temp = cJSON_GetObjectItemCaseSensitive(row, "temp");
                const cJSON *target = cJSON_GetObjectItemCaseSensitive(row, "target");
                const double tmin = ts ? (ts->valuedouble - first_ts) / 60.0 : 0.0;
                last_min = std::max(last_min, static_cast<int>(tmin + 0.5));
                cJSON *p = cJSON_CreateObject();
                cJSON_AddNumberToObject(p, "t", tmin);
                cJSON_AddNumberToObject(p, "temp", target ? target->valuedouble : 0.0);
                cJSON_AddItemToArray(planned, p);
                cJSON *a = cJSON_CreateObject();
                cJSON_AddNumberToObject(a, "t", tmin);
                cJSON_AddNumberToObject(a, "temp", temp ? temp->valuedouble : 0.0);
                cJSON_AddItemToArray(actual, a);
            }
            replace_item(root, "planned", planned);
            replace_item(root, "actual", actual);

            cJSON *summary = cJSON_GetObjectItemCaseSensitive(root, "summary");
            if (!cJSON_IsObject(summary)) {
                summary = cJSON_CreateObject();
                replace_item(root, "summary", summary);
            }
            if (!cJSON_GetObjectItemCaseSensitive(summary, "duration")) {
                cJSON_AddNumberToObject(summary, "duration", last_min);
            }
        } else {
            cJSON *summary = cJSON_GetObjectItemCaseSensitive(root, "summary");
            const cJSON *sn = summary ? cJSON_GetObjectItemCaseSensitive(summary, "scheduleName") : nullptr;
            const cJSON *st = summary ? cJSON_GetObjectItemCaseSensitive(summary, "startTemp") : nullptr;
            const std::string schedule_name = (cJSON_IsString(sn) && sn->valuestring) ? sn->valuestring : "";
            const double start_temp = cJSON_IsNumber(st) ? st->valuedouble : 20.0;
            if (!cJSON_GetObjectItemCaseSensitive(root, "planned")) {
                if (cJSON *planned = build_planned_from_schedule_name(schedule_name, start_temp)) {
                    replace_item(root, "planned", planned);
                }
            }
            if (!cJSON_GetObjectItemCaseSensitive(root, "actual")) {
                replace_item(root, "actual", cJSON_CreateArray());
            }
        }
        char *rendered = cJSON_PrintUnformatted(root);
        std::string output = rendered ? rendered : file;
        if (rendered) free(rendered);
        cJSON_Delete(root);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Pragma", "no-cache");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, output.c_str());
        return ESP_OK;
    }
    if (root) cJSON_Delete(root);

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, file.c_str());
    return ESP_OK;
}
