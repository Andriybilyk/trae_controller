#include "kiln_config/config_store.h"
#include "kiln_config/config.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

static const char *TAG = "CFG_STORE";
static SemaphoreHandle_t s_fs_mutex = xSemaphoreCreateRecursiveMutex();
static QueueHandle_t s_async_save_queue = nullptr;
static constexpr TickType_t kP4SaveDebounceTicks = pdMS_TO_TICKS(1800);

static bool write_string_to_file_locked(const char *path, const std::string &data);
static bool nvs_save_string(const std::string &s);
static std::string migrate_config_json_schema(const std::string &json, bool *changed_out);

struct AsyncSaveReq {
    char *json;
};

static void config_save_worker_task(void *pv) {
    AsyncSaveReq req;
    while (xQueueReceive(s_async_save_queue, &req, portMAX_DELAY)) {
        if (req.json) {
            // Debounce + coalescing:
            // wait for a short quiet window and keep only the latest payload.
            AsyncSaveReq latest_req;
            while (xQueueReceive(s_async_save_queue, &latest_req, kP4SaveDebounceTicks)) {
                free(req.json);
                req = latest_req;
            }

            const std::string migrated = migrate_config_json_schema(req.json, nullptr);
            free(req.json);

            ESP_LOGI(TAG, "Starting debounced async config save to NVS and LittleFS...");
            (void)nvs_save_string(migrated);
            
            SemaphoreHandle_t m = kiln_config_fs_mutex();
            if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
            (void)write_string_to_file_locked(CONFIG_FILE, migrated);
            if (m) xSemaphoreGiveRecursive(m);
            ESP_LOGI(TAG, "Debounced async config save complete");
        }
    }
}

static void ensure_async_worker() {
    if (s_async_save_queue == nullptr) {
        s_async_save_queue = xQueueCreate(8, sizeof(AsyncSaveReq));
        xTaskCreate(config_save_worker_task, "cfg_save", 4096, nullptr, 1, nullptr);
    }
}

static constexpr const char *NVS_NS = "kiln";
static constexpr const char *NVS_KEY = "config_json";
static constexpr int CONFIG_SCHEMA_VERSION = 3;

static std::string to_lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static bool mode_requires_touch_calibration(const char *mode_raw) {
    std::string mode = mode_raw ? mode_raw : "";
    mode = to_lower_ascii(mode);
    if (mode == "headless" || mode == "remote") return false;
    return true; // panel/default
}

SemaphoreHandle_t kiln_config_fs_mutex() {
    return s_fs_mutex;
}

static std::string read_file_to_string_locked(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return {};
    }
    std::string out;
    out.resize((size_t)size);
    const size_t r = fread(out.data(), 1, out.size(), f);
    fclose(f);
    out.resize(r);
    return out;
}

static bool write_string_to_file_locked(const char *path, const std::string &data) {
    const std::string tmp = std::string(path) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    const size_t w = fwrite(data.data(), 1, data.size(), f);
    fflush(f);
    fclose(f);
    if (w != data.size()) {
        (void)remove(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path) != 0) {
        (void)remove(path);
        if (rename(tmp.c_str(), path) != 0) {
            (void)remove(tmp.c_str());
            return false;
        }
    }
    return true;
}

static std::string nvs_load_string() {
    nvs_handle_t h{};
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return {};
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, NVS_KEY, nullptr, &len);
    if (err != ESP_OK || len == 0) {
        nvs_close(h);
        return {};
    }
    std::string out;
    out.resize(len);
    err = nvs_get_str(h, NVS_KEY, out.data(), &len);
    nvs_close(h);
    if (err != ESP_OK || len == 0) return {};
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

static bool nvs_save_string(const std::string &s) {
    nvs_handle_t h{};
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    const esp_err_t err1 = nvs_set_str(h, NVS_KEY, s.c_str());
    const esp_err_t err2 = nvs_commit(h);
    nvs_close(h);
    return err1 == ESP_OK && err2 == ESP_OK;
}

static std::string migrate_config_json_schema(const std::string &json, bool *changed_out = nullptr) {
    bool changed = false;
    cJSON *root = nullptr;
    if (!json.empty()) root = cJSON_ParseWithLength(json.c_str(), json.size());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        root = cJSON_CreateObject();
        changed = true;
    }

    int ver = 1;
    const cJSON *sv = cJSON_GetObjectItem(root, "schema_version");
    if (cJSON_IsNumber(sv)) ver = (int)sv->valuedouble;
    if (ver < 1) ver = 1;

    if (ver < 2) {
        const cJSON *offset = cJSON_GetObjectItem(root, "offset");
        if (cJSON_IsNumber(offset) && !cJSON_IsNumber(cJSON_GetObjectItem(root, "temp_offset_c"))) {
            cJSON_AddNumberToObject(root, "temp_offset_c", offset->valuedouble);
            changed = true;
        }
        const cJSON *at = cJSON_GetObjectItem(root, "autotuneTargetC");
        if (cJSON_IsNumber(at) && !cJSON_IsNumber(cJSON_GetObjectItem(root, "autotune_target_c"))) {
            cJSON_AddNumberToObject(root, "autotune_target_c", at->valuedouble);
            changed = true;
        }
        const cJSON *at2 = cJSON_GetObjectItem(root, "autotune_target");
        if (cJSON_IsNumber(at2) && !cJSON_IsNumber(cJSON_GetObjectItem(root, "autotune_target_c"))) {
            cJSON_AddNumberToObject(root, "autotune_target_c", at2->valuedouble);
            changed = true;
        }
        ver = 2;
        changed = true;
    }

    if (ver < 3) {
        const cJSON *mode_item = cJSON_GetObjectItem(root, "deployment_mode");
        const char *mode = (cJSON_IsString(mode_item) && mode_item->valuestring && mode_item->valuestring[0])
            ? mode_item->valuestring
            : "panel";
        if (!(cJSON_IsString(mode_item) && mode_item->valuestring && mode_item->valuestring[0])) {
            cJSON_DeleteItemFromObject(root, "deployment_mode");
            cJSON_AddStringToObject(root, "deployment_mode", mode);
            changed = true;
        }

        const cJSON *req_touch = cJSON_GetObjectItem(root, "require_touch_calibration_for_start");
        if (!cJSON_IsBool(req_touch)) {
            cJSON_DeleteItemFromObject(root, "require_touch_calibration_for_start");
            cJSON_AddBoolToObject(root, "require_touch_calibration_for_start", mode_requires_touch_calibration(mode));
            changed = true;
        }

        ver = 3;
        changed = true;
    }

    cJSON_DeleteItemFromObject(root, "schema_version");
    cJSON_AddNumberToObject(root, "schema_version", (double)CONFIG_SCHEMA_VERSION);

    char *rendered = cJSON_PrintUnformatted(root);
    std::string out = rendered ? rendered : "{}";
    if (rendered) free(rendered);
    cJSON_Delete(root);
    if (changed_out) *changed_out = changed;
    return out;
}

std::string kiln_config_load_json_config() {
    SemaphoreHandle_t m = kiln_config_fs_mutex();
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    std::string file = read_file_to_string_locked(CONFIG_FILE);
    if (m) xSemaphoreGiveRecursive(m);
    if (!file.empty()) {
        bool migrated = false;
        std::string out = migrate_config_json_schema(file, &migrated);
        if (migrated) (void)kiln_config_save_json_config(out);
        return out;
    }
    std::string nvs = nvs_load_string();
    if (nvs.empty()) return {};
    bool migrated = false;
    std::string out = migrate_config_json_schema(nvs, &migrated);
    if (migrated) (void)kiln_config_save_json_config(out);
    return out;
}

bool kiln_config_save_json_config(const std::string &json) {
#if CONFIG_IDF_TARGET_ESP32P4
    // On ESP32-P4, synchronous flash writes cause visible display flicker.
    // We defer the save to a background task.
    ensure_async_worker();
    AsyncSaveReq req;
    req.json = strdup(json.c_str());
    if (xQueueSend(s_async_save_queue, &req, 0) != pdTRUE) {
        free(req.json);
        return false;
    }
    return true;
#else
    const std::string migrated = migrate_config_json_schema(json, nullptr);
    (void)nvs_save_string(migrated);
    SemaphoreHandle_t m = kiln_config_fs_mutex();
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    const bool ok = write_string_to_file_locked(CONFIG_FILE, migrated);
    if (m) xSemaphoreGiveRecursive(m);
    return ok;
#endif
}

void kiln_config_restore_json_config_file() {
    const std::string json = nvs_load_string();
    if (json.empty()) return;
    const std::string migrated = migrate_config_json_schema(json, nullptr);
    SemaphoreHandle_t m = kiln_config_fs_mutex();
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    FILE *f = fopen(CONFIG_FILE, "r");
    if (f) {
        fclose(f);
        if (m) xSemaphoreGiveRecursive(m);
        return;
    }
    (void)write_string_to_file_locked(CONFIG_FILE, migrated);
    if (m) xSemaphoreGiveRecursive(m);
}
