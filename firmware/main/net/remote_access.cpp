#include "net/remote_access.h"

#include "app/device_commands.h"
#include "kiln_control/thermal_control.h"
#include "kiln_config/fs_utils.h"
#include "net/wifi_connection.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_random.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "psa/crypto.h"

#include "cJSON.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

static const char *TAG = "REMOTE";
static constexpr const char *NVS_NS = "remote_cfg";
static constexpr const char *K_EN = "en";
static constexpr const char *K_URI = "uri";
static constexpr const char *K_USER = "user";
static constexpr const char *K_PASS = "pass";
static constexpr const char *K_DEV = "dev";
static constexpr const char *K_AUTH = "auth";
static constexpr const char *K_REQAUTH = "rqauth";
static constexpr const char *CA_CERT_PATH = "/littlefs/remote_ca.pem";
static constexpr const char *DEFAULT_MQTT_URI = "mqtts://kilnpro:8888";
static constexpr uint64_t AUTH_MAX_SKEW_MS = 5ULL * 60ULL * 1000ULL;
static constexpr size_t AUTH_NONCE_SLOTS = 16;
static const char *DEFAULT_CA_PEM = R"PEM(-----BEGIN CERTIFICATE-----
MIIEAzCCAuugAwIBAgIUBY1hlCGvdj4NhBXkZ/uLUZNILAwwDQYJKoZIhvcNAQEL
BQAwgZAxCzAJBgNVBAYTAkdCMRcwFQYDVQQIDA5Vbml0ZWQgS2luZ2RvbTEOMAwG
A1UEBwwFRGVyYnkxEjAQBgNVBAoMCU1vc3F1aXR0bzELMAkGA1UECwwCQ0ExFjAU
BgNVBAMMDW1vc3F1aXR0by5vcmcxHzAdBgkqhkiG9w0BCQEWEHJvZ2VyQGF0Y2hv
by5vcmcwHhcNMjAwNjA5MTEwNjM5WhcNMzAwNjA3MTEwNjM5WjCBkDELMAkGA1UE
BhMCR0IxFzAVBgNVBAgMDlVuaXRlZCBLaW5nZG9tMQ4wDAYDVQQHDAVEZXJieTES
MBAGA1UECgwJTW9zcXVpdHRvMQswCQYDVQQLDAJDQTEWMBQGA1UEAwwNbW9zcXVp
dHRvLm9yZzEfMB0GCSqGSIb3DQEJARYQcm9nZXJAYXRjaG9vLm9yZzCCASIwDQYJ
KoZIhvcNAQEBBQADggEPADCCAQoCggEBAME0HKmIzfTOwkKLT3THHe+ObdizamPg
UZmD64Tf3zJdNeYGYn4CEXbyP6fy3tWc8S2boW6dzrH8SdFf9uo320GJA9B7U1FW
Te3xda/Lm3JFfaHjkWw7jBwcauQZjpGINHapHRlpiCZsquAthOgxW9SgDgYlGzEA
s06pkEFiMw+qDfLo/sxFKB6vQlFekMeCymjLCbNwPJyqyhFmPWwio/PDMruBTzPH
3cioBnrJWKXc3OjXdLGFJOfj7pP0j/dr2LH72eSvv3PQQFl90CZPFhrCUcRHSSxo
E6yjGOdnz7f6PveLIB574kQORwt8ePn0yidrTC1ictikED3nHYhMUOUCAwEAAaNT
MFEwHQYDVR0OBBYEFPVV6xBUFPiGKDyo5V3+Hbh4N9YSMB8GA1UdIwQYMBaAFPVV
6xBUFPiGKDyo5V3+Hbh4N9YSMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQEL
BQADggEBAGa9kS21N70ThM6/Hj9D7mbVxKLBjVWe2TPsGfbl3rEDfZ+OKRZ2j6AC
6r7jb4TZO3dzF2p6dgbrlU71Y/4K0TdzIjRj3cQ3KSm41JvUQ0hZ/c04iGDg/xWf
+pp58nfPAYwuerruPNWmlStWAXf0UTqRtg4hQDWBuUFDJTuWuuBvEXudz74eh/wK
sMwfu1HFvjy5Z0iMDU8PUDepjVolOCue9ashlS4EB5IECdSR2TItnAIiIwimx839
LdUdRudafMu5T5Xma182OC0/u/xRlEm+tvKGGmfFcN0piqVl8OrSPBgIlb+1IKJE
m/XriWr/Cq4h/JfB7NTsezVslgkBaoU=
-----END CERTIFICATE-----
)PEM";

static remote_access_config_t s_cfg{};
static bool s_cfg_loaded = false;
static bool s_needs_reconnect = false;
static bool s_connected = false;
static bool s_subscribed = false;
static uint64_t s_last_pub_ms = 0;
static esp_mqtt_client_handle_t s_mqtt = nullptr;
static std::string s_topic_cmd;
static std::string s_topic_status;
static std::string s_topic_event;
static std::string s_ca_cert_pem;
static std::string s_nonce_ring[AUTH_NONCE_SLOTS];
static size_t s_nonce_ring_pos = 0;

static bool save_cfg_to_nvs();

static constexpr bool REMOTE_ACCESS_FORCE_DISABLED = true;

static void generate_auth_key(char *out, size_t out_len) {
    if (!out || out_len < 65) return;
    uint8_t rnd[32] = {0};
    esp_fill_random(rnd, sizeof(rnd));
    for (size_t i = 0; i < sizeof(rnd); ++i) {
        std::snprintf(out + (i * 2), 3, "%02x", rnd[i]);
    }
    out[64] = '\0';
}

static bool is_tls_uri(const char *uri) {
    if (!uri || !uri[0]) return false;
    return std::strncmp(uri, "mqtts://", 8) == 0 || std::strncmp(uri, "wss://", 6) == 0;
}

static bool load_ca_cert_from_fs() {
    const std::string pem = kiln_fs_read_text(CA_CERT_PATH);
    if (pem.empty()) {
        s_ca_cert_pem.clear();
        return false;
    }
    s_ca_cert_pem = pem;
    return true;
}

static void set_default_device_id(char *out, size_t out_len) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(out, out_len, "%02X%02X%02X%02X%02X%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void build_topics() {
    const std::string base = std::string("trae/") + s_cfg.device_id;
    s_topic_cmd = base + "/cmd";
    s_topic_status = base + "/status";
    s_topic_event = base + "/event";
}

static bool nvs_read_str(nvs_handle_t h, const char *key, char *out, size_t out_len) {
    if (!out || out_len == 0) return false;
    out[0] = '\0';
    size_t n = out_len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) return false;
    out[out_len - 1] = '\0';
    return out[0] != '\0';
}

static void load_cfg_from_nvs() {
    std::memset(&s_cfg, 0, sizeof(s_cfg));
    set_default_device_id(s_cfg.device_id, sizeof(s_cfg.device_id));
    s_cfg.enabled = false;
    s_cfg.require_signed_commands = true;
    bool cfg_changed = false;
    bool has_enabled_in_nvs = false;

    nvs_handle_t h = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t en = 0;
        if (nvs_get_u8(h, K_EN, &en) == ESP_OK) {
            has_enabled_in_nvs = true;
            s_cfg.enabled = en != 0;
        }
        uint8_t rqauth = 1;
        if (nvs_get_u8(h, K_REQAUTH, &rqauth) == ESP_OK) s_cfg.require_signed_commands = rqauth != 0;
        (void)nvs_read_str(h, K_URI, s_cfg.uri, sizeof(s_cfg.uri));
        (void)nvs_read_str(h, K_USER, s_cfg.username, sizeof(s_cfg.username));
        (void)nvs_read_str(h, K_PASS, s_cfg.password, sizeof(s_cfg.password));
        (void)nvs_read_str(h, K_DEV, s_cfg.device_id, sizeof(s_cfg.device_id));
        (void)nvs_read_str(h, K_AUTH, s_cfg.auth_key, sizeof(s_cfg.auth_key));
        nvs_close(h);
    }

    if (has_enabled_in_nvs && s_cfg.enabled &&
        std::strcmp(s_cfg.uri, DEFAULT_MQTT_URI) == 0 &&
        s_cfg.username[0] == '\0' &&
        s_cfg.password[0] == '\0') {
        s_cfg.enabled = false;
        cfg_changed = true;
    }
    if (!has_enabled_in_nvs) {
        // Default: remote access OFF unless explicitly enabled by user config.
        s_cfg.enabled = false;
        cfg_changed = true;
    }
    if (!s_cfg.uri[0]) {
        std::snprintf(s_cfg.uri, sizeof(s_cfg.uri), "%s", DEFAULT_MQTT_URI);
        cfg_changed = true;
    }
    if (!s_cfg.auth_key[0]) {
        generate_auth_key(s_cfg.auth_key, sizeof(s_cfg.auth_key));
        cfg_changed = true;
    }
    build_topics();
    if (cfg_changed) (void)save_cfg_to_nvs();
    s_cfg_loaded = true;
}

static bool save_cfg_to_nvs() {
    nvs_handle_t h = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;

    (void)nvs_set_u8(h, K_EN, s_cfg.enabled ? 1 : 0);
    (void)nvs_set_str(h, K_URI, s_cfg.uri);
    (void)nvs_set_str(h, K_USER, s_cfg.username);
    (void)nvs_set_str(h, K_PASS, s_cfg.password);
    (void)nvs_set_str(h, K_DEV, s_cfg.device_id);
    (void)nvs_set_str(h, K_AUTH, s_cfg.auth_key);
    (void)nvs_set_u8(h, K_REQAUTH, s_cfg.require_signed_commands ? 1 : 0);
    const esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static void mqtt_publish_event(const char *type, bool ok, const char *msg) {
    if (!s_mqtt || !s_connected || s_topic_event.empty()) return;
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "event", type ? type : "event");
    cJSON_AddBoolToObject(doc, "ok", ok);
    if (msg && msg[0]) cJSON_AddStringToObject(doc, "message", msg);
    char *rendered = cJSON_PrintUnformatted(doc);
    if (rendered) {
        (void)esp_mqtt_client_publish(s_mqtt, s_topic_event.c_str(), rendered, 0, 0, 0);
        free(rendered);
    }
    cJSON_Delete(doc);
}

static void handle_remote_command(const char *payload, int len) {
    std::string body(payload, payload + std::max(0, len));
    cJSON *root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (!root) {
        mqtt_publish_event("cmd", false, "invalid_json");
        return;
    }

    const cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd) || !cmd->valuestring || !cmd->valuestring[0]) {
        cJSON_Delete(root);
        mqtt_publish_event("cmd", false, "missing_cmd");
        return;
    }

    const std::string c = cmd->valuestring;
    bool ok = false;
    std::string msg = "unsupported";

    auto nonce_seen_recently = [](const char *nonce) -> bool {
        if (!nonce || !nonce[0]) return true;
        for (const auto &slot : s_nonce_ring) {
            if (!slot.empty() && slot == nonce) return true;
        }
        return false;
    };

    auto remember_nonce = [](const char *nonce) {
        if (!nonce || !nonce[0]) return;
        s_nonce_ring[s_nonce_ring_pos] = nonce;
        s_nonce_ring_pos = (s_nonce_ring_pos + 1) % AUTH_NONCE_SLOTS;
    };

    auto param_for_signature = [&](const std::string &cmd_name) -> std::string {
        if (cmd_name == "add_temp" || cmd_name == "add_time") {
            const cJSON *delta = cJSON_GetObjectItem(root, "delta");
            if (cJSON_IsNumber(delta)) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.3f", delta->valuedouble);
                return std::string(buf);
            }
            return "missing";
        }
        if (cmd_name == "fan_manual") {
            const cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
            if (cJSON_IsBool(enabled)) return cJSON_IsTrue(enabled) ? "1" : "0";
            return "missing";
        }
        if (cmd_name == "fan_power") {
            const cJSON *power = cJSON_GetObjectItem(root, "power");
            if (cJSON_IsNumber(power)) {
                int32_t pct = (int32_t)power->valuedouble;
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                return std::to_string((int)pct);
            }
            return "missing";
        }
        return "-";
    };

    auto verify_signature = [&](const std::string &cmd_name, std::string &out_msg) -> bool {
        const bool auth_key_present = s_cfg.auth_key[0] != '\0';
        if (!s_cfg.require_signed_commands && !auth_key_present) return true;
        if (!auth_key_present) {
            out_msg = "auth_key_not_set";
            return false;
        }

        const cJSON *ts = cJSON_GetObjectItem(root, "ts_ms");
        const cJSON *nonce = cJSON_GetObjectItem(root, "nonce");
        const cJSON *sig = cJSON_GetObjectItem(root, "sig");
        if (!cJSON_IsNumber(ts) || !cJSON_IsString(nonce) || !nonce->valuestring ||
            !nonce->valuestring[0] || !cJSON_IsString(sig) || !sig->valuestring || !sig->valuestring[0]) {
            out_msg = "missing_auth";
            return false;
        }

        const int64_t ts_ms = (int64_t)ts->valuedouble;
        const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        const uint64_t age_ms = now_ms >= (uint64_t)ts_ms ? (now_ms - (uint64_t)ts_ms) : ((uint64_t)ts_ms - now_ms);
        if (age_ms > AUTH_MAX_SKEW_MS) {
            out_msg = "auth_expired";
            return false;
        }

        if (nonce_seen_recently(nonce->valuestring)) {
            out_msg = "replay_detected";
            return false;
        }

        const std::string material = std::string(s_cfg.device_id) + "|" +
                                     std::to_string(ts_ms) + "|" +
                                     nonce->valuestring + "|" +
                                     cmd_name + "|" +
                                     param_for_signature(cmd_name);

        uint8_t mac[32] = {0};
        size_t mac_len = 0;
        if (psa_crypto_init() != PSA_SUCCESS) {
            out_msg = "auth_internal_error";
            return false;
        }
        psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_HASH);
        psa_set_key_algorithm(&attrs, PSA_ALG_HMAC(PSA_ALG_SHA_256));
        psa_set_key_type(&attrs, PSA_KEY_TYPE_HMAC);
        psa_set_key_bits(&attrs, 8u * (uint32_t)std::strlen(s_cfg.auth_key));
        psa_key_id_t key = 0;
        psa_status_t p = psa_import_key(&attrs,
                                        reinterpret_cast<const uint8_t *>(s_cfg.auth_key),
                                        std::strlen(s_cfg.auth_key),
                                        &key);
        psa_reset_key_attributes(&attrs);
        if (p != PSA_SUCCESS) {
            out_msg = "auth_internal_error";
            return false;
        }
        p = psa_mac_compute(key,
                            PSA_ALG_HMAC(PSA_ALG_SHA_256),
                            reinterpret_cast<const uint8_t *>(material.data()),
                            material.size(),
                            mac,
                            sizeof(mac),
                            &mac_len);
        (void)psa_destroy_key(key);
        if (p != PSA_SUCCESS || mac_len != 32) {
            out_msg = "auth_internal_error";
            return false;
        }

        char expected_hex[65] = {0};
        for (size_t i = 0; i < 32; ++i) std::snprintf(expected_hex + i * 2, 3, "%02x", mac[i]);
        expected_hex[64] = '\0';

        if (std::strcmp(expected_hex, sig->valuestring) != 0) {
            out_msg = "bad_signature";
            return false;
        }

        remember_nonce(nonce->valuestring);
        return true;
    };

    if (!verify_signature(c, msg)) {
        cJSON_Delete(root);
        mqtt_publish_event("cmd", false, msg.c_str());
        return;
    }

    // For safety, start/load schedule is intentionally not exposed remotely.
    if (c == "stop") {
        ok = device_commands::stop("Remote Command").ok();
        msg = ok ? "stopped" : "failed";
    } else if (c == "skip") {
        ok = device_commands::skip().ok();
        msg = ok ? "skipped" : "failed";
    } else if (c == "add_temp") {
        const cJSON *delta = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsNumber(delta)) {
            ok = device_commands::add_temp(delta->valuedouble).ok();
            msg = ok ? "temp_added" : "failed";
        } else {
            msg = "missing_delta";
        }
    } else if (c == "add_time") {
        const cJSON *delta = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsNumber(delta)) {
            ok = device_commands::add_time(delta->valuedouble).ok();
            msg = ok ? "time_added" : "failed";
        } else {
            msg = "missing_delta";
        }
    } else if (c == "fan_manual") {
        const cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
        if (cJSON_IsBool(enabled)) {
            device_commands::set_fan_manual(cJSON_IsTrue(enabled));
            ok = true;
            msg = "fan_manual_set";
        } else {
            msg = "missing_enabled";
        }
    } else if (c == "fan_power") {
        const cJSON *power = cJSON_GetObjectItem(root, "power");
        if (cJSON_IsNumber(power)) {
            int32_t pct = (int32_t)power->valuedouble;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            device_commands::set_fan_power(pct);
            ok = true;
            msg = "fan_power_set";
        } else {
            msg = "missing_power";
        }
    } else if (c == "fault_clear") {
        ok = thermalCtrl.clearFault();
        msg = ok ? "fault_cleared" : "clear_rejected";
    }

    cJSON_Delete(root);
    mqtt_publish_event("cmd", ok, msg.c_str());
}

static void publish_status_snapshot() {
    if (!s_mqtt || !s_connected || s_topic_status.empty()) return;

    const KilnState st = thermalCtrl.getState();
    const SafetyStats safety = thermalCtrl.getSafetyStats();
    const ThermalController::AutoTuneStatus tune = thermalCtrl.getAutotuneStatus();

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "ts_ms", (double)(esp_timer_get_time() / 1000ULL));
    cJSON_AddNumberToObject(doc, "temp", st.currentTemp);
    cJSON_AddNumberToObject(doc, "target", st.targetTemp);
    cJSON_AddNumberToObject(doc, "status", (double)st.status);
    cJSON_AddBoolToObject(doc, "firing", st.isFiring);
    cJSON_AddNumberToObject(doc, "step", st.currentStep);
    cJSON_AddNumberToObject(doc, "total", st.totalSteps);
    cJSON_AddBoolToObject(doc, "wifi_connected", wifi_is_connected());
    cJSON_AddBoolToObject(doc, "fault_active", safety.faultActive);
    if (safety.faultActive) cJSON_AddStringToObject(doc, "fault_reason", safety.faultReason);

    cJSON *a = cJSON_CreateObject();
    cJSON_AddBoolToObject(a, "active", tune.active);
    cJSON_AddNumberToObject(a, "cycles", (double)tune.cycles);
    cJSON_AddNumberToObject(a, "kp", tune.kp);
    cJSON_AddNumberToObject(a, "ki", tune.ki);
    cJSON_AddNumberToObject(a, "kd", tune.kd);
    cJSON_AddItemToObject(doc, "autotune", a);

    char *rendered = cJSON_PrintUnformatted(doc);
    if (rendered) {
        (void)esp_mqtt_client_publish(s_mqtt, s_topic_status.c_str(), rendered, 0, 0, 0);
        free(rendered);
    }
    cJSON_Delete(doc);
}

static void mqtt_destroy() {
    if (!s_mqtt) return;
    (void)esp_mqtt_client_stop(s_mqtt);
    (void)esp_mqtt_client_destroy(s_mqtt);
    s_mqtt = nullptr;
    s_connected = false;
    s_subscribed = false;
}

static void ensure_mqtt_started() {
    if (s_mqtt) return;

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = s_cfg.uri;
    if (is_tls_uri(s_cfg.uri)) {
        if (!load_ca_cert_from_fs()) {
            ESP_LOGE(TAG, "TLS broker configured but CA certificate is missing");
            return;
        }
        cfg.broker.verification.certificate = s_ca_cert_pem.c_str();
    }
    if (s_cfg.username[0]) cfg.credentials.username = s_cfg.username;
    if (s_cfg.password[0]) cfg.credentials.authentication.password = s_cfg.password;
    cfg.session.keepalive = 30;
    cfg.network.disable_auto_reconnect = false;

    s_mqtt = esp_mqtt_client_init(&cfg);
    if (!s_mqtt) {
        ESP_LOGE(TAG, "MQTT init failed");
        return;
    }

    (void)esp_mqtt_client_register_event(
        s_mqtt, MQTT_EVENT_ANY,
        [](void *, esp_event_base_t, int32_t event_id, void *event_data) {
            auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
            switch (event_id) {
                case MQTT_EVENT_CONNECTED:
                    s_connected = true;
                    s_subscribed = false;
                    ESP_LOGI(TAG, "Connected to remote broker");
                    break;
                case MQTT_EVENT_DISCONNECTED:
                    s_connected = false;
                    s_subscribed = false;
                    ESP_LOGW(TAG, "Remote broker disconnected");
                    break;
                case MQTT_EVENT_DATA:
                    if (event && event->topic_len == (int)s_topic_cmd.size() &&
                        std::memcmp(event->topic, s_topic_cmd.data(), s_topic_cmd.size()) == 0) {
                        handle_remote_command(event->data, event->data_len);
                    }
                    break;
                default:
                    break;
            }
        },
        nullptr);

    if (esp_mqtt_client_start(s_mqtt) != ESP_OK) {
        ESP_LOGE(TAG, "MQTT start failed");
        mqtt_destroy();
    }
}

void remote_access_init(void) {
    if (s_cfg_loaded) return;
    load_cfg_from_nvs();
    if (!remote_access_has_ca_pem()) (void)remote_access_set_ca_pem(DEFAULT_CA_PEM);
    if (REMOTE_ACCESS_FORCE_DISABLED && s_cfg.enabled) {
        s_cfg.enabled = false;
        (void)save_cfg_to_nvs();
    }
    ESP_LOGI(TAG, "Remote access initialized (enabled=%d, uri=%s, device=%s)",
             s_cfg.enabled ? 1 : 0, s_cfg.uri, s_cfg.device_id);
}

void remote_access_loop(void) {
    if (!s_cfg_loaded) load_cfg_from_nvs();

    if (REMOTE_ACCESS_FORCE_DISABLED) {
        mqtt_destroy();
        return;
    }

    if (s_needs_reconnect) {
        s_needs_reconnect = false;
        mqtt_destroy();
    }

    if (!s_cfg.enabled || !wifi_is_connected() || s_cfg.uri[0] == '\0' || s_cfg.device_id[0] == '\0') {
        mqtt_destroy();
        return;
    }

    ensure_mqtt_started();
    if (!s_mqtt || !s_connected) return;

    if (!s_subscribed && !s_topic_cmd.empty()) {
        const int mid = esp_mqtt_client_subscribe(s_mqtt, s_topic_cmd.c_str(), 0);
        if (mid >= 0) {
            s_subscribed = true;
            mqtt_publish_event("remote", true, "subscribed");
        }
    }

    const uint64_t now = (uint64_t)(esp_timer_get_time() / 1000ULL);
    if (now - s_last_pub_ms >= 1000) {
        s_last_pub_ms = now;
        publish_status_snapshot();
    }
}

bool remote_access_get_config(remote_access_config_t *out) {
    if (!s_cfg_loaded) load_cfg_from_nvs();
    if (!out) return false;
    *out = s_cfg;
    return true;
}

bool remote_access_set_config(const remote_access_config_t *cfg) {
    if (!cfg) return false;
    if (!s_cfg_loaded) load_cfg_from_nvs();

    s_cfg = *cfg;
    if (REMOTE_ACCESS_FORCE_DISABLED) s_cfg.enabled = false;
    s_cfg.uri[sizeof(s_cfg.uri) - 1] = '\0';
    s_cfg.username[sizeof(s_cfg.username) - 1] = '\0';
    s_cfg.password[sizeof(s_cfg.password) - 1] = '\0';
    s_cfg.device_id[sizeof(s_cfg.device_id) - 1] = '\0';
    s_cfg.auth_key[sizeof(s_cfg.auth_key) - 1] = '\0';
    if (!s_cfg.device_id[0]) set_default_device_id(s_cfg.device_id, sizeof(s_cfg.device_id));

    build_topics();
    if (!save_cfg_to_nvs()) return false;
    s_needs_reconnect = true;
    return true;
}

bool remote_access_is_connected(void) {
    return s_connected;
}

bool remote_access_set_ca_pem(const char *pem) {
    if (!pem || !pem[0]) return false;
    const std::string cert(pem);
    if (cert.find("BEGIN CERTIFICATE") == std::string::npos ||
        cert.find("END CERTIFICATE") == std::string::npos) {
        return false;
    }
    if (!kiln_fs_write_text_atomic(CA_CERT_PATH, cert)) return false;
    s_ca_cert_pem = cert;
    s_needs_reconnect = true;
    return true;
}

bool remote_access_clear_ca_pem(void) {
    std::remove(CA_CERT_PATH);
    s_ca_cert_pem.clear();
    s_needs_reconnect = true;
    return true;
}

bool remote_access_has_ca_pem(void) {
    return !kiln_fs_read_text(CA_CERT_PATH).empty();
}
