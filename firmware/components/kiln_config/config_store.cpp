#include "kiln_config/config_store.h"

#include "kiln_config/config.h"

#include "nvs.h"
#include "nvs_flash.h"

#include <cstdio>

static SemaphoreHandle_t s_fs_mutex = xSemaphoreCreateRecursiveMutex();

static constexpr const char *NVS_NS = "kiln";
static constexpr const char *NVS_KEY = "config_json";

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

std::string kiln_config_load_json_config() {
    SemaphoreHandle_t m = kiln_config_fs_mutex();
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    std::string file = read_file_to_string_locked(CONFIG_FILE);
    if (m) xSemaphoreGiveRecursive(m);
    if (!file.empty()) return file;
    return nvs_load_string();
}

bool kiln_config_save_json_config(const std::string &json) {
    (void)nvs_save_string(json);
    SemaphoreHandle_t m = kiln_config_fs_mutex();
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    const bool ok = write_string_to_file_locked(CONFIG_FILE, json);
    if (m) xSemaphoreGiveRecursive(m);
    return ok;
}

void kiln_config_restore_json_config_file() {
    const std::string json = nvs_load_string();
    if (json.empty()) return;
    SemaphoreHandle_t m = kiln_config_fs_mutex();
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    FILE *f = fopen(CONFIG_FILE, "r");
    if (f) {
        fclose(f);
        if (m) xSemaphoreGiveRecursive(m);
        return;
    }
    (void)write_string_to_file_locked(CONFIG_FILE, json);
    if (m) xSemaphoreGiveRecursive(m);
}
