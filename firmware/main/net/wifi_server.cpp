#include "net/wifi_server.h"
#include "app/device_commands.h"
#include "drivers/display_driver.h"
#include "drivers/fan_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "freertos/task.h"
#include "net/wifi_connection.h"
#include "net/remote_access.h"
#include "net/dns_server.h"
#include "kiln_config/config.h"
#include "kiln_config/config_store.h"
#include "kiln_config/fs_utils.h"
#include "esp_netif.h"
#include "cJSON.h"
#include <string>
#include <cctype>
#include <algorithm>
#include <cmath>
#include <cstring> // For strlen, strstr
#include <cstdio>  // For FILE, fopen, snprintf
#include <cstdlib>
#include <vector>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "SERVER";
static constexpr uint32_t STATE_SCHEMA_VERSION = 1;
static constexpr uint32_t START_RATE_LIMIT_MS = 700;
static constexpr uint32_t STOP_RATE_LIMIT_MS = 400;
static constexpr uint32_t SKIP_RATE_LIMIT_MS = 400;
static constexpr uint32_t TUNE_RATE_LIMIT_MS = 150;
static constexpr const char *AUDIT_LOG_FILE = "/littlefs/logs/audit.log";
static constexpr const char *AUDIT_LOG_PREV_FILE = "/littlefs/logs/audit.log.1";
static constexpr size_t AUDIT_LOG_MAX_BYTES = 128 * 1024;
static constexpr const char *EVENTS_LOG_FILE = "/littlefs/logs/events.log";
static constexpr const char *EVENTS_LOG_PREV_FILE = "/littlefs/logs/events.log.1";
static constexpr const char *SCHEDULES_FILE = "/littlefs/schedules.json";

struct EventLogRow {
    uint64_t ts_ms = 0;
    std::string type;
    std::string message;
    bool fault_like = false;
};

static const char *result_code_to_str(device_commands::ResultCode code);
static const char *result_message(device_commands::ResultCode code);
static std::string normalize_schedule_storage_name(const char *name);
static std::string render_json_and_delete(cJSON *doc);

static std::string schedules_store_load_json() {
    return kiln_fs_read_text(SCHEDULES_FILE);
}

static bool schedules_store_save_json(const std::string &json) {
    return kiln_fs_write_text_atomic(SCHEDULES_FILE, json, true, true);
}

static bool event_or_message_contains_fault(const std::string &value) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower.find("fault") != std::string::npos || lower.find("sensor") != std::string::npos;
}

static bool event_row_is_fault_like(const std::string &type, const std::string &message) {
    const std::string upper = [] (const std::string &v) {
        std::string out = v;
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return out;
    }(type);
    return upper == "FAULT" ||
           upper == "FAULT_CLEAR" ||
           upper == "RECOVERY_RESUME" ||
           upper == "RECOVERY_ABORT" ||
           upper.find("FAULT") != std::string::npos ||
           event_or_message_contains_fault(message);
}

static void append_event_rows_from_file(std::vector<EventLogRow> &rows, const char *path) {
    const std::string content = kiln_fs_read_text(path);
    if (content.empty()) return;

    size_t offset = 0;
    while (offset < content.size()) {
        const size_t line_end = content.find('\n', offset);
        std::string line = line_end == std::string::npos
            ? content.substr(offset)
            : content.substr(offset, line_end - offset);
        offset = line_end == std::string::npos ? content.size() : line_end + 1;
        if (line.empty()) continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        const size_t first_space = line.find(' ');
        if (first_space == std::string::npos) continue;
        const size_t second_space = line.find(' ', first_space + 1);

        EventLogRow row{};
        row.ts_ms = static_cast<uint64_t>(strtoull(line.substr(0, first_space).c_str(), nullptr, 10));
        row.type = second_space == std::string::npos
            ? line.substr(first_space + 1)
            : line.substr(first_space + 1, second_space - first_space - 1);
        row.message = second_space == std::string::npos ? "" : line.substr(second_space + 1);
        row.fault_like = event_row_is_fault_like(row.type, row.message);
        rows.push_back(std::move(row));
    }
}
static void copy_text_field(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src) return;
    std::snprintf(dst, dst_len, "%s", src);
}

static bool is_critical_audit_action(const char *action) {
    if (!action || !action[0]) return false;
    return std::strcmp(action, "start") == 0 ||
           std::strcmp(action, "stop") == 0 ||
           std::strcmp(action, "fault_clear") == 0 ||
           std::strcmp(action, "autotune_start") == 0 ||
           std::strcmp(action, "autotune_stop") == 0;
}

static void rotate_audit_log_if_needed() {
    const size_t size = kiln_fs_file_size(AUDIT_LOG_FILE);
    if (size < AUDIT_LOG_MAX_BYTES) return;
    SemaphoreHandle_t m = kiln_config_fs_mutex();
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    (void)unlink(AUDIT_LOG_PREV_FILE);
    (void)rename(AUDIT_LOG_FILE, AUDIT_LOG_PREV_FILE);
    if (m) xSemaphoreGiveRecursive(m);
}

static void append_audit_log_entry(const char *kind,
                                   const char *action,
                                   const char *source,
                                   bool ok,
                                   const char *code,
                                   const char *message) {
    (void)mkdir(LOGS_DIR, 0777);
    rotate_audit_log_if_needed();

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "ts_ms", (double)(esp_timer_get_time() / 1000ULL));
    cJSON_AddStringToObject(doc, "kind", kind && kind[0] ? kind : "unknown");
    cJSON_AddStringToObject(doc, "action", action && action[0] ? action : "unknown");
    cJSON_AddStringToObject(doc, "source", source && source[0] ? source : "unknown");
    cJSON_AddBoolToObject(doc, "ok", ok);
    cJSON_AddStringToObject(doc, "code", code && code[0] ? code : (ok ? "ok" : "error"));
    cJSON_AddStringToObject(doc, "message", message && message[0] ? message : "");

    char *rendered = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (!rendered) return;

    SemaphoreHandle_t m = kiln_config_fs_mutex();
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    FILE *f = std::fopen(AUDIT_LOG_FILE, "a");
    if (f) {
        std::fwrite(rendered, 1, std::strlen(rendered), f);
        std::fwrite("\n", 1, 1, f);
        std::fflush(f);
        const int fd = fileno(f);
        if (fd >= 0) (void)fsync(fd);
        std::fclose(f);
    }
    if (m) xSemaphoreGiveRecursive(m);
    free(rendered);
}

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

static std::string ap_portal_url() {
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap) {
        esp_netif_ip_info_t ip_info = {};
        if (esp_netif_get_ip_info(ap, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            char url[64] = {0};
            std::snprintf(url, sizeof(url), "http://" IPSTR "/", IP2STR(&ip_info.ip));
            return std::string(url);
        }
    }
    return std::string("http://192.168.4.1/");
}

static bool host_matches_ap_url(const char *host) {
    if (!host || !host[0]) return false;
    const std::string ap_url = ap_portal_url();
    const size_t proto = ap_url.find("://");
    std::string ap_host = (proto == std::string::npos) ? ap_url : ap_url.substr(proto + 3);
    const size_t slash = ap_host.find('/');
    if (slash != std::string::npos) ap_host.resize(slash);
    if (ap_host.empty()) return false;
    return strstr(host, ap_host.c_str()) != NULL;
}

static bool host_is_ip_literal(const char *host) {
    if (!host || !host[0]) return false;
    // Host header can be "ip" or "ip:port"
    for (const char *p = host; *p; ++p) {
        const char c = *p;
        if (c == ':') break;
        if (c != '.' && (c < '0' || c > '9')) return false;
    }
    return true;
}

static bool host_is_connectivity_check(const char *host) {
    if (!host || !host[0]) return false;
    std::string h(host);
    const size_t colon = h.find(':');
    if (colon != std::string::npos) h.resize(colon);
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    return h.find("captive.apple.com") != std::string::npos ||
           h.find("www.apple.com") != std::string::npos ||
           h.find("connectivitycheck.gstatic.com") != std::string::npos ||
           h.find("clients3.google.com") != std::string::npos ||
           h.find("connect.rom.miui.com") != std::string::npos ||
           h.find("msftconnecttest.com") != std::string::npos ||
           h.find("msftncsi.com") != std::string::npos ||
           h.find("detectportal.firefox.com") != std::string::npos;
}

static bool is_captive_probe_uri(const std::string &uri_path) {
    return uri_path == "/generate_204" ||
           uri_path == "/generate204" ||
           uri_path == "/gen_204" ||
           uri_path == "/hotspot-detect.html" ||
           uri_path == "/hotspotdetect.html" ||
           uri_path == "/ncsi.txt" ||
           uri_path == "/connecttest.txt" ||
           uri_path == "/wpad.dat" ||
           uri_path == "/fwlink" ||
           uri_path == "/success.html" ||
           uri_path == "/success.txt" ||
           uri_path == "/canonical.html" ||
           uri_path == "/library/test/success.html" ||
           uri_path.rfind("/browsernetworktime/", 0) == 0;
}

// Mirrors Arduino WebServer captivePortal() behavior:
// if Host is not an IP literal, force 302 redirect to AP IP.
static bool captive_portal_redirect_if_needed(httpd_req_t *req) {
    if (!req) return false;
    if (!wifi_ap_active()) return false;

    char host_str[96] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host_str, sizeof(host_str)) != ESP_OK) {
        return false;
    }
    const std::string uriPath = uri_path_only(req->uri);
    const bool probe_uri = is_captive_probe_uri(uriPath);
    const bool connectivity_host = host_is_connectivity_check(host_str);

    // Do not hijack regular app traffic. Redirect only OS captive probes.
    if (!probe_uri) {
        return false;
    }
    if (host_is_ip_literal(host_str) && !probe_uri) {
        return false;
    }

    const std::string target = ap_portal_url();
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", target.c_str());
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "", 0);

    const int sockfd = httpd_req_to_sockfd(req);
    if (sockfd >= 0) {
        httpd_sess_trigger_close(req->handle, sockfd);
    }
    return true;
}

static esp_err_t captive_portal_api_handler(httpd_req_t *req) {
    const std::string portal = ap_portal_url();
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "captive", true);
    cJSON_AddStringToObject(doc, "user-portal-url", portal.c_str());
    cJSON_AddStringToObject(doc, "venue-info-url", portal.c_str());
    cJSON_AddBoolToObject(doc, "can-extend-session", false);
    cJSON_AddNumberToObject(doc, "seconds-remaining", 0);
    char *body = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/captive+json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return ESP_OK;
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


static const char *get_string_or_null(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) return v->valuestring;
    return nullptr;
}

static std::string humanize_schedule_name(const char *name);

static void normalize_schedule_steps(cJSON *schedule) {
    if (!schedule || !cJSON_IsObject(schedule)) return;

    cJSON *steps = cJSON_GetObjectItem(schedule, "steps");
    if (cJSON_IsArray(steps)) return;

    cJSON *segments = cJSON_GetObjectItem(schedule, "segments");
    if (!cJSON_IsArray(segments)) return;

    cJSON_DetachItemViaPointer(schedule, segments);
    cJSON_AddItemToObject(schedule, "steps", segments);
}

static void ensure_schedule_segments(cJSON *schedule) {
    if (!schedule || !cJSON_IsObject(schedule)) return;
    const cJSON *segments = cJSON_GetObjectItem(schedule, "segments");
    if (cJSON_IsArray(segments)) return;
    const cJSON *steps = cJSON_GetObjectItem(schedule, "steps");
    if (!cJSON_IsArray(steps)) return;
    cJSON_AddItemToObject(schedule, "segments", cJSON_Duplicate(steps, 1));
}

static int json_int_or(const cJSON *obj, const char *key, int fallback) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!v) return fallback;
    if (cJSON_IsNumber(v)) return v->valueint;
    return fallback;
}

static bool has_number(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v);
}

static int extract_hold_minutes(const cJSON *step) {
    if (!step || !cJSON_IsObject(step)) return 0;
    if (has_number(step, "holdTime")) return json_int_or(step, "holdTime", 0);
    if (has_number(step, "hold")) return json_int_or(step, "hold", 0);
    if (has_number(step, "holdMinutes")) return json_int_or(step, "holdMinutes", 0);
    if (has_number(step, "duration")) return json_int_or(step, "duration", 0);
    if (has_number(step, "time")) return json_int_or(step, "time", 0);
    return 0;
}

static void coalesce_web_two_part_steps(cJSON *schedule) {
    if (!schedule || !cJSON_IsObject(schedule)) return;
    cJSON *steps = cJSON_GetObjectItem(schedule, "steps");
    if (!cJSON_IsArray(steps)) return;

    cJSON *merged = cJSON_CreateArray();
    const int n = cJSON_GetArraySize(steps);
    for (int i = 0; i < n; ++i) {
        cJSON *cur = cJSON_GetArrayItem(steps, i);
        if (!cJSON_IsObject(cur)) continue;
        cJSON *cur_dup = cJSON_Duplicate(cur, 1);
        if (!cJSON_IsObject(cur_dup)) continue;

        const int cur_rate = json_int_or(cur_dup, "rate", 0);
        const int cur_target = json_int_or(cur_dup, "target", 0);
        int cur_hold = extract_hold_minutes(cur_dup);

        if (i + 1 < n) {
            cJSON *next = cJSON_GetArrayItem(steps, i + 1);
            if (cJSON_IsObject(next)) {
                const int next_rate = json_int_or(next, "rate", 0);
                const int next_target = json_int_or(next, "target", cur_target);
                int next_hold = extract_hold_minutes(next);

                const bool is_ramp = (cur_rate > 0 && cur_target > 0);
                const bool next_is_hold_part = (next_hold > 0 && next_rate == 0 && next_target == cur_target);
                if (is_ramp && next_is_hold_part) {
                    cJSON_DeleteItemFromObject(cur_dup, "holdTime");
                    cJSON_DeleteItemFromObject(cur_dup, "hold");
                    cJSON_AddNumberToObject(cur_dup, "holdTime", std::max(0, cur_hold + next_hold));
                    ++i;
                }
            }
        }

        cJSON_AddItemToArray(merged, cur_dup);
    }

    cJSON_DeleteItemFromObject(schedule, "steps");
    cJSON_AddItemToObject(schedule, "steps", merged);
}

static void normalize_step_field_aliases(cJSON *schedule) {
    if (!schedule || !cJSON_IsObject(schedule)) return;
    cJSON *steps = cJSON_GetObjectItem(schedule, "steps");
    if (!cJSON_IsArray(steps)) return;

    const int n = cJSON_GetArraySize(steps);
    for (int i = 0; i < n; ++i) {
        cJSON *step = cJSON_GetArrayItem(steps, i);
        if (!cJSON_IsObject(step)) continue;

        const int hold_minutes = extract_hold_minutes(step);
        cJSON_DeleteItemFromObject(step, "holdTime");
        cJSON_DeleteItemFromObject(step, "hold");
        cJSON_DeleteItemFromObject(step, "holdMinutes");
        cJSON_DeleteItemFromObject(step, "duration");
        cJSON_DeleteItemFromObject(step, "time");
        cJSON_AddNumberToObject(step, "holdTime", hold_minutes);
        cJSON_AddNumberToObject(step, "hold", hold_minutes);
        cJSON_AddNumberToObject(step, "holdMinutes", hold_minutes);
        cJSON_AddNumberToObject(step, "duration", hold_minutes);
        cJSON_AddNumberToObject(step, "time", hold_minutes);
    }
}

static void expand_hold_for_web_response(cJSON *schedule) {
    if (!schedule || !cJSON_IsObject(schedule)) return;
    cJSON *steps = cJSON_GetObjectItem(schedule, "steps");
    if (!cJSON_IsArray(steps)) return;

    cJSON *expanded = cJSON_CreateArray();
    const int n = cJSON_GetArraySize(steps);
    for (int i = 0; i < n; ++i) {
        cJSON *step = cJSON_GetArrayItem(steps, i);
        if (!cJSON_IsObject(step)) continue;
        cJSON *ramp = cJSON_Duplicate(step, 1);
        if (!cJSON_IsObject(ramp)) continue;

        const int hold = extract_hold_minutes(step);
        const int target = json_int_or(step, "target", 0);
        const int rate = json_int_or(step, "rate", 0);
        const bool has_hold = hold > 0;
        const bool is_hold_only = (rate == 0 && target > 0 && has_hold);

        if (!is_hold_only) {
            cJSON_DeleteItemFromObject(ramp, "holdTime");
            cJSON_DeleteItemFromObject(ramp, "hold");
            cJSON_DeleteItemFromObject(ramp, "holdMinutes");
            cJSON_DeleteItemFromObject(ramp, "duration");
            cJSON_DeleteItemFromObject(ramp, "time");
            cJSON_AddNumberToObject(ramp, "holdTime", 0);
            cJSON_AddStringToObject(ramp, "type", "ramp");
            cJSON_AddItemToArray(expanded, ramp);

            if (has_hold) {
                cJSON *hold_step = cJSON_CreateObject();
                cJSON_AddStringToObject(hold_step, "type", "hold");
                cJSON_AddNumberToObject(hold_step, "rate", 0);
                cJSON_AddNumberToObject(hold_step, "target", target);
                cJSON_AddNumberToObject(hold_step, "holdTime", hold);
                cJSON_AddNumberToObject(hold_step, "hold", hold);
                cJSON_AddItemToArray(expanded, hold_step);
            }
        } else {
            cJSON_AddItemToArray(expanded, ramp);
        }
    }

    cJSON_DeleteItemFromObject(schedule, "steps");
    cJSON_AddItemToObject(schedule, "steps", expanded);
    ensure_schedule_segments(schedule);
}

static std::string ensure_schedule_name(cJSON *schedule) {
    if (!schedule || !cJSON_IsObject(schedule)) return {};

    const char *existingName = get_string_or_null(schedule, "name");
    const char *title = get_string_or_null(schedule, "title");
    const char *id = get_string_or_null(schedule, "id");

    std::string chosen;
    if (existingName) {
        chosen = existingName;
    } else if (title) {
        chosen = title;
    } else if (id) {
        chosen = id;
    }
    chosen = normalize_schedule_storage_name(chosen.c_str());

    if (!chosen.empty()) {
        if (!existingName || strcmp(existingName, chosen.c_str()) != 0) {
            cJSON_DeleteItemFromObject(schedule, "name");
            cJSON_AddStringToObject(schedule, "name", chosen.c_str());
        }
        return chosen;
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

    std::string id(name);
    for (char &ch : id) {
        if (ch == ' ') ch = '_';
    }
    cJSON_DeleteItemFromObject(schedule, "id");
    cJSON_AddStringToObject(schedule, "id", id.c_str());
}

static std::string humanize_schedule_name(const char *name) {
    if (!name) return {};
    std::string out(name);
    for (char &c : out) {
        if (c == '_') c = ' ';
    }
    return out;
}

static std::string normalize_schedule_storage_name(const char *name) {
    if (!name) return {};
    std::string out;
    out.reserve(strlen(name));
    bool prev_underscore = false;
    for (char c : std::string(name)) {
        const bool is_ws = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (is_ws) {
            if (!prev_underscore) {
                out.push_back('_');
                prev_underscore = true;
            }
            continue;
        }
        out.push_back(c);
        prev_underscore = (c == '_');
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

static std::string normalize_schedule_name_key(const char *name) {
    if (!name) return {};
    std::string out;
    out.reserve(strlen(name));
    for (char c : std::string(name)) {
        if (c == '_' || c == ' ') continue;
        out.push_back((char)std::tolower((unsigned char)c));
    }
    return out;
}

static cJSON *build_normalized_schedule_copy(cJSON *source) {
    if (!cJSON_IsObject(source)) return nullptr;

    cJSON *schedule_source = source;
    cJSON *wrapped = cJSON_GetObjectItem(source, "schedule");
    if (cJSON_IsObject(wrapped)) {
        schedule_source = wrapped;
    }

    cJSON *normalized = cJSON_Duplicate(schedule_source, 1);
    if (!cJSON_IsObject(normalized)) {
        if (normalized) cJSON_Delete(normalized);
        return nullptr;
    }

    cJSON *steps_top = cJSON_GetObjectItem(source, "steps");
    if (!cJSON_IsArray(steps_top)) steps_top = cJSON_GetObjectItem(source, "segments");
    if (cJSON_IsArray(steps_top)) {
        cJSON_DeleteItemFromObject(normalized, "steps");
        cJSON_AddItemToObject(normalized, "steps", cJSON_Duplicate(steps_top, 1));
        cJSON_DeleteItemFromObject(normalized, "segments");
    } else {
        cJSON *segments = cJSON_GetObjectItem(normalized, "segments");
        cJSON *steps = cJSON_GetObjectItem(normalized, "steps");
        if (cJSON_IsArray(segments) && cJSON_IsArray(steps)) {
            cJSON_DeleteItemFromObject(normalized, "segments");
        }
    }

    normalize_schedule_steps(normalized);
    coalesce_web_two_part_steps(normalized);
    normalize_step_field_aliases(normalized);
    ensure_schedule_segments(normalized);
    ensure_schedule_name(normalized);
    ensure_schedule_id(normalized);
    return normalized;
}

esp_err_t static_asset_handler(httpd_req_t *req);
esp_err_t redirect_handler(httpd_req_t *req);
static esp_err_t captive_probe_handler(httpd_req_t *req);
static esp_err_t send_captive_bootstrap(httpd_req_t *req);
static esp_err_t wifi_setup_page_handler(httpd_req_t *req);
static esp_err_t send_wifi_setup_file(httpd_req_t *req);
static esp_err_t send_captive_plain_page(httpd_req_t *req);
static esp_err_t send_captive_probe_page(httpd_req_t *req);
static esp_err_t catch_all_post_handler(httpd_req_t *req);

static esp_err_t send_captive_plain_page(httpd_req_t *req) {
    const std::string target = ap_portal_url();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    const std::string body =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Wi-Fi Sign In</title></head><body>"
        "<h3>TRAE Wi-Fi Setup</h3>"
        "<p>This network requires setup.</p>"
        "<a href='" + target + "'>Open Wi-Fi Manager</a>"
        "</body></html>";
    httpd_resp_send(req, body.c_str(), body.size());
    return ESP_OK;
}

static esp_err_t wifi_setup_page_handler(httpd_req_t *req) {
    if (captive_portal_redirect_if_needed(req)) {
        return ESP_OK;
    }
    return send_wifi_setup_file(req);
}

static esp_err_t send_wifi_setup_file(httpd_req_t *req) {
    const char *path = "/littlefs/wifi_setup.html";
    FILE *f = fopen(path, "r");
    if (!f) {
        return redirect_handler(req);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    if (req->method == HTTP_HEAD) {
        fclose(f);
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    char chunk[512];
    size_t chunksize = 0;
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

static esp_err_t send_captive_bootstrap(httpd_req_t *req) {
    const std::string portal_url = ap_portal_url();
    const std::string target = portal_url;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    const std::string body =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>TRAE Wi-Fi Setup</title>"
        "<meta http-equiv='refresh' content='0; url=" + target + "'>"
        "</head><body>"
        "<p>Opening Wi-Fi setup...</p>"
        "<script>location.replace('" + target + "');</script>"
        "<a href='" + target + "'>Open Wi-Fi setup</a>"
        "</body></html>";
    httpd_resp_send(req, body.c_str(), body.size());
    return ESP_OK;
}

static esp_err_t send_captive_probe_page(httpd_req_t *req) {
    const std::string target = ap_portal_url();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");

    const std::string body =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Wi-Fi Sign In</title>"
        "<meta http-equiv='refresh' content='0; url=" + target + "'>"
        "</head><body>"
        "<script>location.replace('" + target + "');</script>"
        "<a href='" + target + "'>Open Wi-Fi setup</a>"
        "</body></html>";
    httpd_resp_send(req, body.c_str(), body.size());
    return ESP_OK;
}

esp_err_t WiFiServerManager::index_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "index_handler called for URI: %s", req->uri);

    if (captive_portal_redirect_if_needed(req)) {
        return ESP_OK;
    }

    const std::string uriPath = uri_path_only(req->uri);
    if (strncmp(req->uri, "/api/", 5) == 0 || strncmp(req->uri, "/assets/", 8) == 0) {
        ESP_LOGW(TAG, "index_handler rejecting /api/ or /assets/ request: %s", req->uri);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char host_str[64] = {0};
    const bool has_host = (httpd_req_get_hdr_value_str(req, "Host", host_str, sizeof(host_str)) == ESP_OK);
    if (has_host) {
        ESP_LOGI(TAG, "Host header: %s", host_str);
    }
    const bool ap_active = wifi_ap_active();
    const bool captive_probe = is_captive_probe_uri(uriPath);
    const bool ap_host = has_host && host_matches_ap_url(host_str);
    const bool ip_literal_host = has_host && host_is_ip_literal(host_str);
    const bool connectivity_host = has_host && host_is_connectivity_check(host_str);
    const bool ap_provision_mode = wifiServer.isAPMode;
    const bool host_missing_on_ap = ap_provision_mode && ap_active && !has_host;
    // If request explicitly targets AP host (192.168.4.1), always serve Wi-Fi setup page.
    // This guarantees manual open of http://192.168.4.1 works even if higher-level state flags drift.
    if (ap_active && ap_host) {
        return wifi_setup_page_handler(req);
    }
    // iOS/Android connectivity checks often hit "/" with a special Host.
    // Explicit 302 redirect to AP URL is the most deterministic captive trigger for CNA.
    if (ap_provision_mode && ap_active && connectivity_host) {
        return redirect_handler(req);
    }

    const bool serve_portal = ap_provision_mode && (captive_probe || ap_host || host_missing_on_ap || connectivity_host);
    if (serve_portal) {
        if (connectivity_host) {
            return wifi_setup_page_handler(req);
        }
        // Closer to Arduino captive examples:
        // non-IP foreign hosts should get an explicit redirect to AP portal.
        if (ap_provision_mode && ap_active && has_host && !ip_literal_host && !ap_host) {
            return redirect_handler(req);
        }
        // Root requests in portal context should render setup page directly.
        if (uriPath == "/" || uriPath.empty() || uriPath == "/index.html") {
            return wifi_setup_page_handler(req);
        }
        return send_captive_bootstrap(req);
    }

    if (uriPath.find('.') != std::string::npos) {
        return static_asset_handler(req);
    }

    const char* path = "/littlefs/index.html";
    if (ap_provision_mode && ap_active && has_host && !ap_host) {
        ESP_LOGI(TAG, "AP host mismatch in index: host=%s -> redirect", host_str);
        return redirect_handler(req);
    }

    ESP_LOGI(TAG, "Serving file: %s", path);
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "File not found: %s", path);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
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

esp_err_t static_asset_handler(httpd_req_t *req) {
    if (captive_portal_redirect_if_needed(req)) {
        return ESP_OK;
    }
    const std::string uriPath = uri_path_only(req->uri);
    if (wifiServer.isAPMode && wifi_ap_active() && (uriPath == "/index.html" || uriPath == "/")) {
        return wifi_setup_page_handler(req);
    }

    if (strstr(uriPath.c_str(), ".js")) httpd_resp_set_type(req, "application/javascript");
    else if (strstr(uriPath.c_str(), ".css")) httpd_resp_set_type(req, "text/css");
    else if (strstr(uriPath.c_str(), ".html")) httpd_resp_set_type(req, "text/html; charset=utf-8");
    else if (strstr(uriPath.c_str(), ".json")) httpd_resp_set_type(req, "application/json");
    else if (strstr(uriPath.c_str(), ".ico")) httpd_resp_set_type(req, "image/x-icon");
    else if (strstr(uriPath.c_str(), ".svg")) httpd_resp_set_type(req, "image/svg+xml");
    else if (strstr(uriPath.c_str(), ".woff2")) httpd_resp_set_type(req, "font/woff2");
    else if (strstr(uriPath.c_str(), ".woff")) httpd_resp_set_type(req, "font/woff");
    else if (strstr(uriPath.c_str(), ".ttf")) httpd_resp_set_type(req, "font/ttf");

    char filepath[600];
    snprintf(filepath, sizeof(filepath), "/littlefs%s", uriPath.c_str());

    FILE* f = fopen(filepath, "rb");
    if (f == NULL) {
        char gz_filepath[604];
        snprintf(gz_filepath, sizeof(gz_filepath), "%s.gz", filepath);
        f = fopen(gz_filepath, "rb");
        if (f) {
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        }
    }

    if (f == NULL) {
        const bool ap_portal_mode = wifi_ap_active() && wifiServer.isAPMode;
        ESP_LOGW(TAG, "Asset not found: uri=%s file=%s portal=%d", req->uri, filepath, (int)ap_portal_mode);
        if (ap_portal_mode && (req->method == HTTP_GET || req->method == HTTP_HEAD)) {
            return redirect_handler(req);
        }
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_send(req, "Asset not found", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

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
    ESP_LOGI(TAG, "redirect_handler called for URI: %s", req->uri);
    httpd_resp_set_status(req, "302 Found");
    const std::string portal_url = ap_portal_url();
    const std::string target = portal_url;
    httpd_resp_set_hdr(req, "Location", target.c_str());
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    if (req->method == HTTP_HEAD) {
        httpd_resp_send(req, NULL, 0);
    } else {
        const std::string body = "<html><body><a href='" + target + "'>Redirect</a></body></html>";
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, body.c_str(), body.size());
    }
    return ESP_OK;
}

static esp_err_t captive_probe_handler(httpd_req_t *req) {
    const std::string uriPath = uri_path_only(req->uri);
    char host_str[96] = {0};
    char ua_str[128] = {0};
    const bool has_host = (httpd_req_get_hdr_value_str(req, "Host", host_str, sizeof(host_str)) == ESP_OK);
    const bool has_ua = (httpd_req_get_hdr_value_str(req, "User-Agent", ua_str, sizeof(ua_str)) == ESP_OK);
    ESP_LOGI(TAG, "captive_probe: uri=%s host=%s ua=%s method=%d",
             uriPath.c_str(),
             has_host ? host_str : "-",
             has_ua ? ua_str : "-",
             (int)req->method);

    // iOS CNA is sensitive to hotspot-detect/success responses.
    // For these URIs return a small non-success HTML page (not "Success"),
    // and auto-open AP root. For others keep explicit redirect.
    const bool ios_probe_html =
        (uriPath == "/hotspot-detect.html") ||
        (uriPath == "/hotspotdetect.html") ||
        (uriPath == "/success.html") ||
        (uriPath == "/library/test/success.html");

    const std::string target = ap_portal_url();
    if (ios_probe_html) {
        return redirect_handler(req);
    } else {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", target.c_str());
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Pragma", "no-cache");
        httpd_resp_set_hdr(req, "Connection", "close");
        if (req->method == HTTP_HEAD) {
            httpd_resp_send(req, NULL, 0);
        } else {
            httpd_resp_set_type(req, "text/html; charset=utf-8");
            const std::string body = "<html><body><a href='" + target + "'>Open Wi-Fi setup</a></body></html>";
            httpd_resp_send(req, body.c_str(), body.size());
        }
    }
    const int sockfd = httpd_req_to_sockfd(req);
    if (sockfd >= 0) {
        httpd_sess_trigger_close(req->handle, sockfd);
    }
    return ESP_OK;
}

static esp_err_t catch_all_post_handler(httpd_req_t *req) {
    (void)req;
    return redirect_handler(req);
}

static esp_err_t vite_handler(httpd_req_t *req) {
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t WiFiServerManager::scan_wifi_handler(httpd_req_t *req) {
    if (!wifi_backend_available()) {
        ESP_LOGW(TAG, "WiFi scan requested, but backend is unavailable");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"wifi_backend_unavailable\"}");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Starting async WiFi scan");
    wifi_start_scan();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"scanning_started\"}");
    return ESP_OK;
}

esp_err_t WiFiServerManager::scan_results_handler(httpd_req_t *req) {
    if (!wifi_backend_available()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"wifi_backend_unavailable\"}");
        return ESP_OK;
    }
    if (wifi_is_scan_done()) {
        std::string json = wifi_get_scanned_networks();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json.c_str());
        ESP_LOGI(TAG, "Sent scan results");
    } else {
        std::string cached = wifi_get_scanned_networks();
        if (!cached.empty() && cached != "[]") {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, cached.c_str());
        } else {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"status\":\"scanning\"}");
        }
    }
    return ESP_OK;
}

esp_err_t WiFiServerManager::save_wifi_handler(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len > 1024) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"invalid_payload\"}");
        return ESP_OK;
    }

    std::string body;
    body.resize((size_t)req->content_len);
    int off = 0;
    int remaining = req->content_len;
    while (remaining > 0) {
        const int ret = httpd_req_recv(req, body.data() + off, remaining);
        if (ret <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"recv_failed\"}");
            return ESP_OK;
        }
        off += ret;
        remaining -= ret;
    }
    body.resize((size_t)off);

    auto parse_form_field = [&](const char *key, char *out, size_t out_len) {
        (void)out_len;
        if (!key || !out || out_len == 0) return;
        std::string pattern = std::string(key) + "=";
        const size_t begin = body.find(pattern);
        if (begin == std::string::npos) return;
        const size_t value_begin = begin + pattern.size();
        const size_t value_end = body.find('&', value_begin);
        const std::string encoded = body.substr(value_begin,
                                                value_end == std::string::npos ? std::string::npos : (value_end - value_begin));
        if (encoded.empty()) return;
        std::vector<char> tmp(encoded.begin(), encoded.end());
        tmp.push_back('\0');
        url_decode(out, tmp.data());
    };

    char ssid[33] = {0};
    char password[65] = {0};
    parse_form_field("ssid", ssid, sizeof(ssid));
    parse_form_field("password", password, sizeof(password));

    if (!ssid[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"missing_ssid\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "save_wifi: connect requested for SSID='%s'", ssid);
    const bool started = wifi_connect_with_credentials(ssid, password);
    httpd_resp_set_type(req, "application/json");
    if (started) {
        httpd_resp_sendstr(req, "{\"status\":\"connecting\"}");
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"connect_start_failed\"}");
    }
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
                const auto res = device_commands::load_schedule_json(msg);
                wifiServer.notifyCommandResult("load_schedule", res, "loaded", "ws");
            } else if ((cJSON_IsString(command) && command->valuestring && strcmp(command->valuestring, "stop") == 0) ||
                       (cJSON_IsString(action) && action->valuestring && strcmp(action->valuestring, "stop") == 0)) {
                const auto res = device_commands::stop("Web Request");
                wifiServer.notifyCommandResult("stop", res, "stopped", "ws");
            }

            cJSON_Delete(root);
        } else if (msg == "stop" || msg == "STOP") {
            const auto res = device_commands::stop("Web Request");
            wifiServer.notifyCommandResult("stop", res, "stopped", "ws");
        }
    }

    free(buf);
    return ESP_OK;
}

void WiFiServerManager::begin() {
#if CONFIG_IDF_TARGET_ESP32P4
    // On ESP32-P4, bringing up WiFi stack immediately causes bus contention 
    // that interferes with initial MIPI display sequence.
    // We defer the actual hardware and server initialization to loop() where it can 
    // wait for UI stability.
    ESP_LOGI(TAG, "WiFi and Server init deferred for P4 boot stability");
#else
    // Always bring up SoftAP immediately so UI/captive portal is available.
    // STA autoconnect is started asynchronously a bit later to avoid early boot races.
    wifi_init_softap();
    isAPMode = true;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    // We register many GET/HEAD pairs + API routes + captive endpoints.
    // Keep enough slots to avoid silent route drops ("no slots left").
    config.max_uri_handlers = 320;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server, &config) == ESP_OK) {
        setupRoutes();
    } else {
        ESP_LOGE(TAG, "httpd_start failed");
    }
#endif
}

void WiFiServerManager::setupRoutes() {
    httpd_uri_t capport_api_get = make_uri("/captive-portal/api", HTTP_GET, captive_portal_api_handler);
    httpd_register_uri_handler(server, &capport_api_get);
    httpd_uri_t capport_api_head = make_uri("/captive-portal/api", HTTP_HEAD, captive_portal_api_handler);
    httpd_register_uri_handler(server, &capport_api_head);
    // RFC 8908 well-known alias used by some captive portal clients.
    httpd_uri_t capport_wk_get = make_uri("/.well-known/captive-portal", HTTP_GET, captive_portal_api_handler);
    httpd_register_uri_handler(server, &capport_wk_get);
    httpd_uri_t capport_wk_head = make_uri("/.well-known/captive-portal", HTTP_HEAD, captive_portal_api_handler);
    httpd_register_uri_handler(server, &capport_wk_head);

    httpd_uri_t portal_get = make_uri("/portal", HTTP_GET, redirect_handler);
    httpd_register_uri_handler(server, &portal_get);
    httpd_uri_t portal_head = make_uri("/portal", HTTP_HEAD, redirect_handler);
    httpd_register_uri_handler(server, &portal_head);

    httpd_uri_t wifi_setup_html_get = make_uri("/wifi_setup.html", HTTP_GET, wifi_setup_page_handler);
    httpd_register_uri_handler(server, &wifi_setup_html_get);
    httpd_uri_t wifi_setup_html_head = make_uri("/wifi_setup.html", HTTP_HEAD, wifi_setup_page_handler);
    httpd_register_uri_handler(server, &wifi_setup_html_head);
    httpd_uri_t wifi_setup_get = make_uri("/wifi_setup", HTTP_GET, wifi_setup_page_handler);
    httpd_register_uri_handler(server, &wifi_setup_get);
    httpd_uri_t wifi_setup_head = make_uri("/wifi_setup", HTTP_HEAD, wifi_setup_page_handler);
    httpd_register_uri_handler(server, &wifi_setup_head);

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

    httpd_uri_t api_history_clear_uri = make_uri("/api/history", HTTP_DELETE, api_history_clear_handler);
    httpd_register_uri_handler(server, &api_history_clear_uri);

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

    httpd_uri_t api_set_rate_uri = make_uri("/api/setRate", HTTP_POST, api_set_rate_handler);
    httpd_register_uri_handler(server, &api_set_rate_uri);

    httpd_uri_t api_fan_get_uri = make_uri("/api/fan", HTTP_GET, api_fan_get_handler);
    httpd_register_uri_handler(server, &api_fan_get_uri);

    httpd_uri_t api_fan_set_uri = make_uri("/api/fan", HTTP_POST, api_fan_set_handler);
    httpd_register_uri_handler(server, &api_fan_set_uri);

    httpd_uri_t api_ota_update_uri = make_uri("/api/ota/update", HTTP_POST, api_ota_update_handler);
    httpd_register_uri_handler(server, &api_ota_update_uri);

    httpd_uri_t api_fault_clear_uri = make_uri("/api/fault/clear", HTTP_POST, api_fault_clear_handler);
    httpd_register_uri_handler(server, &api_fault_clear_uri);

    httpd_uri_t api_fault_get_uri = make_uri("/api/fault", HTTP_GET, api_fault_get_handler);
    httpd_register_uri_handler(server, &api_fault_get_uri);

    httpd_uri_t api_events_uri = make_uri("/api/events", HTTP_GET, api_events_handler);
    httpd_register_uri_handler(server, &api_events_uri);

    httpd_uri_t api_display_get_uri = make_uri("/api/display", HTTP_GET, api_display_get_handler);
    httpd_register_uri_handler(server, &api_display_get_uri);

    httpd_uri_t api_display_set_uri = make_uri("/api/display", HTTP_POST, api_display_set_handler);
    httpd_register_uri_handler(server, &api_display_set_uri);

    httpd_uri_t api_settings_get_uri = make_uri("/api/settings", HTTP_GET, api_settings_get_handler);
    httpd_register_uri_handler(server, &api_settings_get_uri);

    httpd_uri_t api_settings_set_uri = make_uri("/api/settings", HTTP_POST, api_settings_set_handler);
    httpd_register_uri_handler(server, &api_settings_set_uri);

    httpd_uri_t settings_get_uri = make_uri("/settings", HTTP_GET, index_handler);
    httpd_register_uri_handler(server, &settings_get_uri);

    httpd_uri_t settings_set_uri = make_uri("/settings", HTTP_POST, api_settings_set_handler);
    httpd_register_uri_handler(server, &settings_set_uri);

    httpd_uri_t api_backup_export_uri = make_uri("/api/backup", HTTP_GET, api_backup_export_handler);
    httpd_register_uri_handler(server, &api_backup_export_uri);

    httpd_uri_t api_backup_import_uri = make_uri("/api/backup/import", HTTP_POST, api_backup_import_handler);
    httpd_register_uri_handler(server, &api_backup_import_uri);

    httpd_uri_t api_diag_bundle_uri = make_uri("/api/diagnostics/bundle", HTTP_GET, api_diagnostics_bundle_handler);
    httpd_register_uri_handler(server, &api_diag_bundle_uri);

    httpd_uri_t api_remote_get_uri = make_uri("/api/remote", HTTP_GET, api_remote_get_handler);
    httpd_register_uri_handler(server, &api_remote_get_uri);

    httpd_uri_t api_remote_set_uri = make_uri("/api/remote", HTTP_POST, api_remote_set_handler);
    httpd_register_uri_handler(server, &api_remote_set_uri);

    httpd_uri_t api_pid_get_uri = make_uri("/api/pid", HTTP_GET, api_pid_get_handler);
    httpd_register_uri_handler(server, &api_pid_get_uri);

    httpd_uri_t api_pid_post_uri = make_uri("/api/pid", HTTP_POST, api_pid_post_handler);
    httpd_register_uri_handler(server, &api_pid_post_uri);

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

    httpd_uri_t api_touch_affine_get_uri = make_uri("/api/touch/affine", HTTP_GET, api_touch_affine_get_handler);
    httpd_register_uri_handler(server, &api_touch_affine_get_uri);

    httpd_uri_t api_touch_affine_set_uri = make_uri("/api/touch/affine", HTTP_POST, api_touch_affine_set_handler);
    httpd_register_uri_handler(server, &api_touch_affine_set_uri);

    httpd_uri_t api_touch_grid_get_uri = make_uri("/api/touch/grid", HTTP_GET, api_touch_grid_get_handler);
    httpd_register_uri_handler(server, &api_touch_grid_get_uri);

    httpd_uri_t api_touch_grid_set_uri = make_uri("/api/touch/grid", HTTP_POST, api_touch_grid_set_handler);
    httpd_register_uri_handler(server, &api_touch_grid_set_uri);

    httpd_uri_t api_touch_profile_get_uri = make_uri("/api/touch/profile", HTTP_GET, api_touch_profile_get_handler);
    httpd_register_uri_handler(server, &api_touch_profile_get_uri);

    httpd_uri_t api_touch_profile_set_uri = make_uri("/api/touch/profile", HTTP_POST, api_touch_profile_set_handler);
    httpd_register_uri_handler(server, &api_touch_profile_set_uri);

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

    httpd_uri_t touch_calib_uri = make_uri("/touch_calibration.html", HTTP_GET, static_asset_handler);
    httpd_register_uri_handler(server, &touch_calib_uri);
    
    httpd_uri_t vite_uri = make_uri("/@vite/client", HTTP_GET, vite_handler);
    httpd_register_uri_handler(server, &vite_uri);
    
    httpd_uri_t assets_uri = make_uri("/assets/*", HTTP_GET, static_asset_handler);
    httpd_register_uri_handler(server, &assets_uri);
    httpd_uri_t assets_uri_head = make_uri("/assets/*", HTTP_HEAD, static_asset_handler);
    httpd_register_uri_handler(server, &assets_uri_head);

    if (isAPMode) {
        httpd_uri_t generate_204 = make_uri("/generate_204", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &generate_204);
        httpd_uri_t generate_204_head = make_uri("/generate_204", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &generate_204_head);

        httpd_uri_t gen_204 = make_uri("/gen_204", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &gen_204);
        httpd_uri_t gen_204_head = make_uri("/gen_204", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &gen_204_head);

        httpd_uri_t generate204 = make_uri("/generate204", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &generate204);
        httpd_uri_t generate204_head = make_uri("/generate204", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &generate204_head);

        httpd_uri_t hotspot_detect = make_uri("/hotspot-detect.html", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &hotspot_detect);
        httpd_uri_t hotspot_detect_head = make_uri("/hotspot-detect.html", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &hotspot_detect_head);

        httpd_uri_t hotspotdetect = make_uri("/hotspotdetect.html", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &hotspotdetect);
        httpd_uri_t hotspotdetect_head = make_uri("/hotspotdetect.html", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &hotspotdetect_head);

        httpd_uri_t ncsi = make_uri("/ncsi.txt", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &ncsi);
        httpd_uri_t ncsi_head = make_uri("/ncsi.txt", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &ncsi_head);

        httpd_uri_t connecttest = make_uri("/connecttest.txt", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &connecttest);
        httpd_uri_t connecttest_head = make_uri("/connecttest.txt", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &connecttest_head);

        httpd_uri_t wpad = make_uri("/wpad.dat", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &wpad);
        httpd_uri_t wpad_head = make_uri("/wpad.dat", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &wpad_head);

        httpd_uri_t browsernetworktime = make_uri("/browsernetworktime/*", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &browsernetworktime);
        httpd_uri_t browsernetworktime_head = make_uri("/browsernetworktime/*", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &browsernetworktime_head);

        httpd_uri_t connectivity_check = make_uri("/connectivity-check.html", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &connectivity_check);
        httpd_uri_t connectivity_check_head = make_uri("/connectivity-check.html", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &connectivity_check_head);

        httpd_uri_t check_network_status = make_uri("/check_network_status.txt", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &check_network_status);
        httpd_uri_t check_network_status_head = make_uri("/check_network_status.txt", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &check_network_status_head);

        httpd_uri_t mobile_status = make_uri("/mobile/status.php", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &mobile_status);
        httpd_uri_t mobile_status_head = make_uri("/mobile/status.php", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &mobile_status_head);

        httpd_uri_t kindle_stub = make_uri("/kindle-wifi/wifistub.html", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &kindle_stub);
        httpd_uri_t kindle_stub_head = make_uri("/kindle-wifi/wifistub.html", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &kindle_stub_head);

        httpd_uri_t fwlink = make_uri("/fwlink", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &fwlink);
        httpd_uri_t fwlink_head = make_uri("/fwlink", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &fwlink_head);

        httpd_uri_t redirect_get = make_uri("/redirect", HTTP_GET, redirect_handler);
        httpd_register_uri_handler(server, &redirect_get);
        httpd_uri_t redirect_head = make_uri("/redirect", HTTP_HEAD, redirect_handler);
        httpd_register_uri_handler(server, &redirect_head);

        httpd_uri_t success_txt = make_uri("/success.txt", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &success_txt);
        httpd_uri_t success_txt_head = make_uri("/success.txt", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &success_txt_head);

        httpd_uri_t canonical = make_uri("/canonical.html", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &canonical);
        httpd_uri_t canonical_head = make_uri("/canonical.html", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &canonical_head);

        httpd_uri_t apple_success = make_uri("/library/test/success.html", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &apple_success);
        httpd_uri_t apple_success_head = make_uri("/library/test/success.html", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &apple_success_head);
        httpd_uri_t success_html = make_uri("/success.html", HTTP_GET, captive_probe_handler);
        httpd_register_uri_handler(server, &success_html);
        httpd_uri_t success_html_head = make_uri("/success.html", HTTP_HEAD, captive_probe_handler);
        httpd_register_uri_handler(server, &success_html_head);

        // Mirror esp-idf-wifi-provisioner behavior:
        // Unknown methods/paths in AP mode are redirected to AP root.
        httpd_uri_t catch_all_post = make_uri("/*", HTTP_POST, catch_all_post_handler);
        httpd_register_uri_handler(server, &catch_all_post);
    }

    httpd_uri_t index_uri = make_uri("/*", HTTP_GET, index_handler);
    httpd_register_uri_handler(server, &index_uri);
    httpd_uri_t index_uri_head = make_uri("/*", HTTP_HEAD, index_handler);
    httpd_register_uri_handler(server, &index_uri_head);
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

esp_err_t WiFiServerManager::api_events_handler(httpd_req_t *req) {
    std::vector<EventLogRow> rows;
    append_event_rows_from_file(rows, EVENTS_LOG_PREV_FILE);
    append_event_rows_from_file(rows, EVENTS_LOG_FILE);

    std::sort(rows.begin(), rows.end(), [](const EventLogRow &a, const EventLogRow &b) {
        return a.ts_ms > b.ts_ms;
    });

    int limit = 200;
    std::string limit_q;
    if (query_value(req, "limit", limit_q) && !limit_q.empty()) {
        const int parsed = atoi(limit_q.c_str());
        if (parsed > 0) limit = std::min(parsed, 1000);
    }

    cJSON *doc = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    const size_t take = std::min(rows.size(), static_cast<size_t>(limit));
    for (size_t i = 0; i < take; ++i) {
        const EventLogRow &row = rows[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "ts_ms", (double)row.ts_ms);
        cJSON_AddStringToObject(item, "type", row.type.c_str());
        cJSON_AddStringToObject(item, "message", row.message.c_str());
        cJSON_AddBoolToObject(item, "fault_like", row.fault_like);
        cJSON_AddItemToArray(items, item);
    }
    cJSON_AddItemToObject(doc, "items", items);
    cJSON_AddNumberToObject(doc, "count", (double)take);
    cJSON_AddNumberToObject(doc, "total", (double)rows.size());

    std::string output = render_json_and_delete(doc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, output.c_str());
    return ESP_OK;
}

void WiFiServerManager::loop() {
    uint64_t now = esp_timer_get_time() / 1000;

    static bool s_p4_hw_init_done = false;
#if !CONFIG_IDF_TARGET_ESP32P4
    s_p4_hw_init_done = true;
#endif

    // If STA init failed during early boot, retry after UI/server are already running.
    // This avoids "connect only after scan button" behavior when backend comes up late.
    static uint64_t s_last_sta_diag_ms = 0;
    static uint64_t s_next_sta_attempt_ms = 0;
    static bool s_sta_initial_delay_armed = false;
    static constexpr uint64_t kStaInitialDelayMs = 2500;
    static constexpr uint64_t kStaRetryDelayMs = 10000;

    const uint64_t ui_hb = now;
    const bool ui_ready = true;

    // On P4, wait for UI to be stable (at least 3 seconds of heartbeat) before 
    // kicking off the SDIO WiFi stack.
    if (!s_p4_hw_init_done && ui_hb > 3000) {
        ESP_LOGI(TAG, "P4 UI stable (hb=%llu), starting WiFi HW...", (unsigned long long)ui_hb);
        wifi_init_softap();
        isAPMode = true;

        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.stack_size = 8192;
        config.lru_purge_enable = true;
        config.max_uri_handlers = 320;
        config.uri_match_fn = httpd_uri_match_wildcard;

        if (httpd_start(&server, &config) == ESP_OK) {
            setupRoutes();
        } else {
            ESP_LOGE(TAG, "httpd_start failed during deferred init");
        }

        s_p4_hw_init_done = true;
    }

    if (!s_p4_hw_init_done) return;

    // If STA init failed during early boot, retry after UI/server are already running.
    const bool configured = wifi_is_configured();
    const bool connected = wifi_is_connected();
    const bool scan_busy = wifi_scan_in_progress();
    const bool connect_busy = wifi_sta_connect_in_progress();
    const bool user_enabled = wifi_is_user_enabled();
    static bool s_prev_user_enabled = true;

    if (!user_enabled) {
        if (s_prev_user_enabled) {
            wifi_disconnect_sta();
            wifi_init_softap();
            isAPMode = true;
        }
        s_prev_user_enabled = false;
    } else {
        s_prev_user_enabled = true;
    }

    static uint64_t s_connected_since_ms = 0;
    static constexpr uint64_t kApShutdownGraceMs = 15000;
    if (connected) {
        if (s_connected_since_ms == 0) {
            s_connected_since_ms = now;
        }
        if (wifi_ap_active() && (now - s_connected_since_ms) >= kApShutdownGraceMs) {
            wifi_set_sta_only_mode();
        }
        isAPMode = wifi_ap_active();
    } else {
        s_connected_since_ms = 0;
    }

    if (!connected && (s_last_sta_diag_ms == 0 || (now - s_last_sta_diag_ms) >= 5000)) {
        s_last_sta_diag_ms = now;
        ESP_LOGI(TAG, "AUTO_STA gate: ui_ready=%d configured=%d connected=%d scan_busy=%d connect_busy=%d ui_hb=%llu",
                 ui_ready ? 1 : 0,
                 configured ? 1 : 0,
                 connected ? 1 : 0,
                 scan_busy ? 1 : 0,
                 connect_busy ? 1 : 0,
                 (unsigned long long)ui_hb);
    }

    if (!s_sta_initial_delay_armed && ui_ready) {
        s_sta_initial_delay_armed = true;
        s_next_sta_attempt_ms = now + kStaInitialDelayMs;
    }

    if (user_enabled && ui_ready && configured && !connected && !scan_busy && !connect_busy) {
        if (s_next_sta_attempt_ms == 0 || now >= s_next_sta_attempt_ms) {
            if (wifi_init_sta()) {
                s_next_sta_attempt_ms = now + kStaRetryDelayMs;
                ESP_LOGI(TAG, "AUTO_STA: connect started");
            } else {
                s_next_sta_attempt_ms = now + kStaRetryDelayMs;
                ESP_LOGW(TAG, "AUTO_STA: connect deferred (not ready/no creds)");
            }
        }
    } else if (connected) {
        s_next_sta_attempt_ms = now + kStaRetryDelayMs;
    }

    if (now - lastBroadcast > 1000) {
        lastBroadcast = now;
        broadcastState();

        const auto tune = thermalCtrl.getAutotuneStatus();
        const bool prevTuneActive = lastTuneActive.load(std::memory_order_relaxed);
        const int prevTuneCycles = lastTuneCycles.load(std::memory_order_relaxed);
        if (tune.active != prevTuneActive || tune.cycles != prevTuneCycles) {
            lastTuneActive.store(tune.active, std::memory_order_relaxed);
            lastTuneCycles.store(tune.cycles, std::memory_order_relaxed);
            notifyAutotuneState(tune.active ? "progress" : "inactive");
        }
    }
}

static const char* status_to_str(int s) {
    switch(s) {
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

static cJSON *build_device_state_doc() {
    KilnState state = thermalCtrl.getState();
    const ThermoStats thermo = thermalCtrl.getThermoStats();
    const SafetyStats safety = thermalCtrl.getSafetyStats();
    const ThermalController::AutoTuneStatus tune = thermalCtrl.getAutotuneStatus();
    const esp_app_desc_t *app = esp_app_get_description();
    const std::string server_url = wifi_get_server_url();

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "ts_ms", (double)(esp_timer_get_time() / 1000ULL));
    cJSON_AddNumberToObject(doc, "uptime_ms", (double)(esp_timer_get_time() / 1000ULL));
    cJSON_AddNumberToObject(doc, "schema_version", (double)STATE_SCHEMA_VERSION);
    cJSON_AddStringToObject(doc, "fw_version", (app && app->version[0]) ? app->version : "unknown");

    cJSON_AddNumberToObject(doc, "temp", state.currentTemp);
    cJSON_AddNumberToObject(doc, "raw", thermo.raw);
    cJSON_AddNumberToObject(doc, "read_count", (double)thermo.readCount);
    cJSON_AddNumberToObject(doc, "target", state.targetTemp);
    cJSON_AddNumberToObject(doc, "pcbTemp", (double)fan_driver_get_source_temp_c());
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
    cJSON_AddBoolToObject(doc, "fan_manual", fan_driver_get_manual());
    cJSON_AddBoolToObject(doc, "fan_auto", fan_driver_get_auto_enabled());
    cJSON_AddNumberToObject(doc, "fan_power", (double)fan_driver_get_power_percent());
    cJSON_AddNumberToObject(doc, "fan_effective_power", (double)fan_driver_get_effective_power_percent());
    cJSON_AddBoolToObject(doc, "wifi_connected", wifi_is_connected());
    cJSON_AddStringToObject(doc, "server_url", server_url.c_str());

    cJSON *t = cJSON_CreateObject();
    cJSON_AddBoolToObject(t, "active", tune.active);
    cJSON_AddBoolToObject(t, "heater_on", tune.heaterOn);
    cJSON_AddNumberToObject(t, "setpoint_c", (double)tune.setpointC);
    cJSON_AddNumberToObject(t, "cycles", (double)tune.cycles);
    cJSON_AddNumberToObject(t, "valid_cycles", (double)tune.valid_cycles);
    cJSON_AddNumberToObject(t, "total_cycles", (double)tune.total_cycles);
    cJSON_AddNumberToObject(t, "ku", (double)tune.ku);
    cJSON_AddNumberToObject(t, "pu_s", (double)tune.pu_s);
    cJSON_AddNumberToObject(t, "period_cv", (double)tune.period_cv);
    cJSON_AddNumberToObject(t, "amp_cv", (double)tune.amp_cv);
    cJSON_AddNumberToObject(t, "quality", (double)tune.quality);
    cJSON_AddNumberToObject(t, "confidence", (double)tune.confidence);
    cJSON_AddNumberToObject(t, "kp", (double)tune.kp);
    cJSON_AddNumberToObject(t, "ki", (double)tune.ki);
    cJSON_AddNumberToObject(t, "kd", (double)tune.kd);
    cJSON_AddItemToObject(doc, "autotune", t);
    return doc;
}

static std::string render_json_and_delete(cJSON *doc) {
    if (!doc) return "{}";
    char *rendered = cJSON_PrintUnformatted(doc);
    std::string out = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(doc);
    return out;
}

void WiFiServerManager::broadcastState() {
    if (!server) return;
    
    // Check if there are any connected websocket clients before building JSON
    size_t clients = 10;
    int client_fds[10];
    if (httpd_get_client_list(server, &clients, client_fds) != ESP_OK || clients == 0) {
        return;
    }

    bool has_ws_clients = false;
    for (size_t i = 0; i < clients; i++) {
        if (httpd_ws_get_fd_info(server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            has_ws_clients = true;
            break;
        }
    }

    if (!has_ws_clients) return;

    std::string output = render_json_and_delete(build_device_state_doc());

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)output.c_str();
    ws_pkt.len = output.length();
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    for (size_t i = 0; i < clients; i++) {
        if (httpd_ws_get_fd_info(server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            httpd_ws_send_frame_async(server, client_fds[i], &ws_pkt);
            taskYIELD();
        }
    }
}

bool WiFiServerManager::copyLastCommandResult(command_result_snapshot_t *out) const {
    if (!out) return false;
    std::lock_guard<std::mutex> guard(commandResultMutex);
    *out = lastCommandResult;
    return true;
}

void WiFiServerManager::notifyCommandResult(const char *action,
                                            const device_commands::CommandResult &result,
                                            const char *ok_message,
                                            const char *source) {
    const uint64_t ts_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    const bool ok = result.ok();
    const char *code = result_code_to_str(result.code);
    const char *message = ok ? (ok_message ? ok_message : "ok") : result_message(result.code);

    uint32_t rev = 0;
    {
        std::lock_guard<std::mutex> guard(commandResultMutex);
        rev = commandResultRevision.fetch_add(1, std::memory_order_relaxed) + 1;
        if (rev == 0) {
            commandResultRevision.store(1, std::memory_order_relaxed);
            rev = 1;
        }

        lastCommandResult.valid = true;
        lastCommandResult.ok = ok;
        lastCommandResult.rev = rev;
        lastCommandResult.ts_ms = ts_ms;
        copy_text_field(lastCommandResult.action, sizeof(lastCommandResult.action), action ? action : "unknown");
        copy_text_field(lastCommandResult.source, sizeof(lastCommandResult.source), source ? source : "unknown");
        copy_text_field(lastCommandResult.code, sizeof(lastCommandResult.code), code);
        copy_text_field(lastCommandResult.message, sizeof(lastCommandResult.message), message);
    }
    if (is_critical_audit_action(action)) {
        append_audit_log_entry("command", action, source, ok, code, message);
    }

    if (!server) return;

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "event", "command_result");
    cJSON_AddNumberToObject(doc, "rev", (double)rev);
    cJSON_AddNumberToObject(doc, "ts_ms", (double)ts_ms);
    cJSON_AddStringToObject(doc, "action", action && action[0] ? action : "unknown");
    cJSON_AddStringToObject(doc, "source", source && source[0] ? source : "unknown");
    cJSON_AddBoolToObject(doc, "ok", ok);
    cJSON_AddStringToObject(doc, "code", code ? code : (ok ? "ok" : "internal"));
    cJSON_AddStringToObject(doc, "message", message ? message : "");

    std::string output = render_json_and_delete(doc);

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t *)output.c_str();
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
    uint32_t rev = schedulesRevision.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rev == 0) {
        schedulesRevision.store(1, std::memory_order_relaxed);
        rev = 1;
    }
    if (!server) return;

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "event", "schedules_changed");
    cJSON_AddNumberToObject(doc, "rev", (double)rev);
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
    uint32_t rev = settingsRevision.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rev == 0) {
        settingsRevision.store(1, std::memory_order_relaxed);
        rev = 1;
    }
    append_audit_log_entry("settings", action && action[0] ? action : "settings", "api", true, "ok", "settings_changed");
    if (!server) return;

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "event", "settings_changed");
    cJSON_AddNumberToObject(doc, "rev", (double)rev);
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
    const ThermalController::AutoTuneStatus tune = thermalCtrl.getAutotuneStatus();
    const SafetyStats safety = thermalCtrl.getSafetyStats();
    if (action && action[0]) {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "active=%d,setpoint=%.1f,cycles=%d,fault=%d",
                      tune.active ? 1 : 0, (double)tune.setpointC, tune.cycles, safety.faultActive ? 1 : 0);
        append_audit_log_entry("autotune", action, "state", !safety.faultActive, safety.faultActive ? "fault" : "ok", msg);
    }
    if (!server) return;

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
    cJSON_AddNumberToObject(t, "valid_cycles", (double)tune.valid_cycles);
    cJSON_AddNumberToObject(t, "total_cycles", (double)tune.total_cycles);
    cJSON_AddNumberToObject(t, "ku", (double)tune.ku);
    cJSON_AddNumberToObject(t, "pu_s", (double)tune.pu_s);
    cJSON_AddNumberToObject(t, "period_cv", (double)tune.period_cv);
    cJSON_AddNumberToObject(t, "amp_cv", (double)tune.amp_cv);
    cJSON_AddNumberToObject(t, "quality", (double)tune.quality);
    cJSON_AddNumberToObject(t, "confidence", (double)tune.confidence);
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
    std::string output = render_json_and_delete(build_device_state_doc());
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

    const std::string file = schedules_store_load_json();
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
        const std::string wanted_key = normalize_schedule_name_key(name.c_str());
        cJSON *match = nullptr;
        cJSON *it = nullptr;
        cJSON_ArrayForEach(it, root) {
            const cJSON *n = cJSON_GetObjectItem(it, "name");
            const cJSON *idv = cJSON_GetObjectItem(it, "id");
            if (cJSON_IsString(n) && n->valuestring && strcmp(n->valuestring, name.c_str()) == 0) {
                match = it;
                break;
            }
            if (cJSON_IsString(idv) && idv->valuestring && strcmp(idv->valuestring, name.c_str()) == 0) {
                match = it;
                break;
            }
            if (!wanted_key.empty()) {
                const bool name_match = cJSON_IsString(n) && n->valuestring &&
                    normalize_schedule_name_key(n->valuestring) == wanted_key;
                const bool id_match = cJSON_IsString(idv) && idv->valuestring &&
                    normalize_schedule_name_key(idv->valuestring) == wanted_key;
                if (name_match || id_match) {
                    match = it;
                    break;
                }
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
        normalize_step_field_aliases(match);
        expand_hold_for_web_response(match);
        ensure_schedule_segments(match);
        ensure_schedule_id(match);
        if (const cJSON *n = cJSON_GetObjectItem(match, "name"); cJSON_IsString(n) && n->valuestring) {
            cJSON_DeleteItemFromObject(match, "title");
            const std::string title = humanize_schedule_name(n->valuestring);
            cJSON_AddStringToObject(match, "title", title.c_str());
        }
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
            normalize_step_field_aliases(it);
            expand_hold_for_web_response(it);
            ensure_schedule_segments(it);

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
                cJSON_DeleteItemFromObject(it, "title");
                cJSON_AddStringToObject(it, "title", humanize_schedule_name(fallback).c_str());
            } else {
                cJSON_DeleteItemFromObject(it, "title");
                const std::string title = humanize_schedule_name(n->valuestring);
                cJSON_AddStringToObject(it, "title", title.c_str());
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
        for (int i = 0; i < cJSON_GetArraySize(incoming); ++i) {
            cJSON *item = cJSON_GetArrayItem(incoming, i);
            cJSON *normalized = build_normalized_schedule_copy(item);
            if (!normalized) continue;
            cJSON_ReplaceItemInArray(incoming, i, normalized);
        }
        arr = incoming; // overwrite
        incoming = nullptr;
    } else if (cJSON_IsObject(incoming)) {
        cJSON *scheduleObj = build_normalized_schedule_copy(incoming);
        if (!scheduleObj) {
            cJSON_Delete(incoming);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\":\"invalid_schedule\"}");
            return ESP_OK;
        }
        respName = ensure_schedule_name(scheduleObj);

        const std::string existingFile = schedules_store_load_json();
        cJSON *root = existingFile.empty() ? cJSON_CreateArray() : cJSON_Parse(existingFile.c_str());
        if (!root || !cJSON_IsArray(root)) {
            if (root) cJSON_Delete(root);
            root = cJSON_CreateArray();
        }

        const char *name = respName.c_str();
        const char *incoming_id = get_string_or_null(scheduleObj, "id");
        const std::string incoming_name_key = normalize_schedule_name_key(name);
        bool replaced = false;
        for (int i = 0; i < cJSON_GetArraySize(root); i++) {
            cJSON *it = cJSON_GetArrayItem(root, i);
            const cJSON *n = cJSON_GetObjectItem(it, "name");
            const cJSON *id = cJSON_GetObjectItem(it, "id");
            const bool id_match = incoming_id && cJSON_IsString(id) && id->valuestring && strcmp(id->valuestring, incoming_id) == 0;
            const bool name_match = cJSON_IsString(n) && n->valuestring && strcmp(n->valuestring, name) == 0;
            const bool id_missing = !(cJSON_IsString(id) && id->valuestring && id->valuestring[0]);
            const bool legacy_key_match =
                id_missing &&
                !incoming_name_key.empty() &&
                cJSON_IsString(n) &&
                n->valuestring &&
                normalize_schedule_name_key(n->valuestring) == incoming_name_key;
            if (id_match || name_match || legacy_key_match) {
                cJSON_ReplaceItemInArray(root, i, cJSON_Duplicate(scheduleObj, 1));
                replaced = true;
                break;
            }
        }
        if (!replaced) cJSON_AddItemToArray(root, cJSON_Duplicate(scheduleObj, 1));

        cJSON_Delete(scheduleObj);
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

    if (!schedules_store_save_json(out)) {
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

    const std::string existingFile = schedules_store_load_json();
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

    if (!schedules_store_save_json(out)) {
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

static const char *result_code_to_str(device_commands::ResultCode code) {
    switch (code) {
        case device_commands::ResultCode::Ok: return "ok";
        case device_commands::ResultCode::InvalidPayload: return "invalid_payload";
        case device_commands::ResultCode::InvalidSchedule: return "invalid_schedule";
        case device_commands::ResultCode::InvalidState: return "invalid_state";
        case device_commands::ResultCode::NoSchedule: return "no_schedule";
        case device_commands::ResultCode::SensorInvalid: return "sensor_invalid";
        case device_commands::ResultCode::TouchNotCalibrated: return "touch_not_calibrated";
        case device_commands::ResultCode::FaultActive: return "fault_active";
        case device_commands::ResultCode::FanUnsafe: return "fan_unsafe";
        case device_commands::ResultCode::ScheduleOverMaxTemp: return "schedule_over_max_temp";
        case device_commands::ResultCode::ClockNotSet: return "clock_not_set";
        default: return "internal";
    }
}

static const char *result_message(device_commands::ResultCode code) {
    switch (code) {
        case device_commands::ResultCode::Ok: return "ok";
        case device_commands::ResultCode::InvalidPayload: return "Invalid payload";
        case device_commands::ResultCode::InvalidSchedule: return "Invalid schedule";
        case device_commands::ResultCode::InvalidState: return "Command not allowed in current state";
        case device_commands::ResultCode::NoSchedule: return "No schedule loaded";
        case device_commands::ResultCode::SensorInvalid: return "Sensor invalid";
        case device_commands::ResultCode::TouchNotCalibrated: return "Touch not calibrated";
        case device_commands::ResultCode::FaultActive: return "Fault active";
        case device_commands::ResultCode::FanUnsafe: return "Fan configuration unsafe";
        case device_commands::ResultCode::ScheduleOverMaxTemp: return "Schedule exceeds max temperature";
        case device_commands::ResultCode::ClockNotSet: return "RTC/system clock not set";
        default: return "Internal error";
    }
}

static void send_command_envelope(httpd_req_t *req,
                                  bool ok,
                                  const char *code,
                                  const char *message,
                                  const char *http_status = nullptr) {
    if (http_status && http_status[0]) {
        httpd_resp_set_status(req, http_status);
    }
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "ok", ok);
    cJSON_AddStringToObject(doc, "code", code ? code : (ok ? "ok" : "internal"));
    cJSON_AddStringToObject(doc, "message", message ? message : "");
    std::string out = render_json_and_delete(doc);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out.c_str());
}

static bool check_rate_limit(uint64_t &last_ms, uint32_t window_ms, httpd_req_t *req) {
    const uint64_t now = (uint64_t)(esp_timer_get_time() / 1000ULL);
    if (last_ms > 0 && now > last_ms && (now - last_ms) < window_ms) {
        send_command_envelope(req, false, "rate_limited", "Too many requests", "429 Too Many Requests");
        return false;
    }
    last_ms = now;
    return true;
}

static void send_command_result(httpd_req_t *req,
                                const device_commands::CommandResult &result,
                                const char *ok_message,
                                const char *action,
                                const char *source = "api") {
    wifiServer.notifyCommandResult(action, result, ok_message, source);

    switch (result.code) {
        case device_commands::ResultCode::Ok:
            send_command_envelope(req, true, "ok", ok_message ? ok_message : "ok");
            break;
        case device_commands::ResultCode::InvalidPayload:
            send_command_envelope(req, false, result_code_to_str(result.code), result_message(result.code), "400 Bad Request");
            break;
        case device_commands::ResultCode::InvalidSchedule:
            send_command_envelope(req, false, result_code_to_str(result.code), result_message(result.code), "400 Bad Request");
            break;
        case device_commands::ResultCode::InvalidState:
            send_command_envelope(req, false, result_code_to_str(result.code), result_message(result.code), "409 Conflict");
            break;
        case device_commands::ResultCode::NoSchedule:
            send_command_envelope(req, false, result_code_to_str(result.code), result_message(result.code), "409 Conflict");
            break;
        case device_commands::ResultCode::SensorInvalid:
            send_command_envelope(req, false, result_code_to_str(result.code), result_message(result.code), "409 Conflict");
            break;
        case device_commands::ResultCode::TouchNotCalibrated:
            send_command_envelope(req, false, result_code_to_str(result.code), result_message(result.code), "409 Conflict");
            break;
        case device_commands::ResultCode::FaultActive:
        case device_commands::ResultCode::FanUnsafe:
        case device_commands::ResultCode::ScheduleOverMaxTemp:
        case device_commands::ResultCode::ClockNotSet:
            send_command_envelope(req, false, result_code_to_str(result.code), result_message(result.code), "409 Conflict");
            break;
        default:
            send_command_envelope(req, false, "internal", "Internal error", "500 Internal Server Error");
            break;
    }
}

static bool parse_numeric_delta_from_body(const std::string &body, const char *json_key, double &out) {
    if (body.empty()) return false;

    char *end = nullptr;
    const double raw = strtod(body.c_str(), &end);
    if (end != body.c_str()) {
        out = raw;
        return true;
    }

    cJSON *root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return false;
    }

    const cJSON *v = cJSON_GetObjectItem(root, json_key);
    const bool ok = cJSON_IsNumber(v);
    if (ok) out = v->valuedouble;
    cJSON_Delete(root);
    return ok;
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
    if (!check_rate_limit(wifiServer.lastStartCommandMs, START_RATE_LIMIT_MS, req)) {
        return ESP_OK;
    }

    std::string payload;
    payload.resize((size_t)req->content_len);
    size_t off = 0;
    while (off < payload.size()) {
        const int r = httpd_req_recv(req, payload.data() + off, payload.size() - off);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        off += (size_t)r;
    }

    const device_commands::CommandResult res = payload.empty()
        ? device_commands::start_loaded_schedule()
        : device_commands::start_from_api_payload(payload);
    send_command_result(req, res, "started", "start");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_stop_handler(httpd_req_t *req) {
    if (!check_rate_limit(wifiServer.lastStopCommandMs, STOP_RATE_LIMIT_MS, req)) {
        return ESP_OK;
    }
    send_command_result(req, device_commands::stop("API Request"), "stopped", "stop");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_skip_handler(httpd_req_t *req) {
    if (!check_rate_limit(wifiServer.lastSkipCommandMs, SKIP_RATE_LIMIT_MS, req)) {
        return ESP_OK;
    }
    send_command_result(req, device_commands::skip(), "skip_received", "skip");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_add_temp_handler(httpd_req_t *req) {
    if (!check_rate_limit(wifiServer.lastAddTempCommandMs, TUNE_RATE_LIMIT_MS, req)) {
        return ESP_OK;
    }
    std::string body;
    body.resize((size_t)req->content_len);
    size_t off = 0;
    while (off < body.size()) {
        const int r = httpd_req_recv(req, body.data() + off, body.size() - off);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            send_command_result(req, {device_commands::ResultCode::InvalidPayload}, "ok", "add_temp");
            return ESP_OK;
        }
        off += (size_t)r;
    }
    double delta = 0.0;
    if (!parse_numeric_delta_from_body(body, "degrees", delta)) {
        send_command_result(req, {device_commands::ResultCode::InvalidPayload}, "ok", "add_temp");
        return ESP_OK;
    }
    send_command_result(req, device_commands::add_temp(delta), "ok", "add_temp");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_add_time_handler(httpd_req_t *req) {
    if (!check_rate_limit(wifiServer.lastAddTimeCommandMs, TUNE_RATE_LIMIT_MS, req)) {
        return ESP_OK;
    }
    std::string body;
    body.resize((size_t)req->content_len);
    size_t off = 0;
    while (off < body.size()) {
        const int r = httpd_req_recv(req, body.data() + off, body.size() - off);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            send_command_result(req, {device_commands::ResultCode::InvalidPayload}, "ok", "add_time");
            return ESP_OK;
        }
        off += (size_t)r;
    }
    double delta = 0.0;
    if (!parse_numeric_delta_from_body(body, "minutes", delta)) {
        send_command_result(req, {device_commands::ResultCode::InvalidPayload}, "ok", "add_time");
        return ESP_OK;
    }
    send_command_result(req, device_commands::add_time(delta), "ok", "add_time");
    return ESP_OK;
}

esp_err_t WiFiServerManager::api_set_rate_handler(httpd_req_t *req) {
    if (!check_rate_limit(wifiServer.lastSetRateCommandMs, TUNE_RATE_LIMIT_MS, req)) {
        return ESP_OK;
    }
    std::string body;
    body.resize((size_t)req->content_len);
    size_t off = 0;
    while (off < body.size()) {
        const int r = httpd_req_recv(req, body.data() + off, body.size() - off);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            send_command_result(req, {device_commands::ResultCode::InvalidPayload}, "ok", "set_rate");
            return ESP_OK;
        }
        off += (size_t)r;
    }
    double rate = 0.0;
    if (!parse_numeric_delta_from_body(body, "rate", rate)) {
        send_command_result(req, {device_commands::ResultCode::InvalidPayload}, "ok", "set_rate");
        return ESP_OK;
    }
    send_command_result(req, device_commands::set_rate(rate), "ok", "set_rate");
    return ESP_OK;
}

WiFiServerManager wifiServer;
