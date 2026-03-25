#include "net/wifi_server.h"

#include "psa/crypto.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cctype>
#include <string>

static constexpr size_t OTA_MAX_IMAGE_BYTES = 6 * 1024 * 1024;

static bool normalize_sha256_hex(const std::string &input, std::string &out_lower_hex) {
    out_lower_hex.clear();
    for (char c : input) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        out_lower_hex.push_back((char)std::tolower(static_cast<unsigned char>(c)));
    }
    if (out_lower_hex.size() != 64) return false;
    for (char c : out_lower_hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

static std::string sha256_to_hex(const uint8_t digest[32]) {
    static const char *kHex = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (size_t i = 0; i < 32; ++i) {
        out[2 * i] = kHex[(digest[i] >> 4) & 0x0F];
        out[2 * i + 1] = kHex[digest[i] & 0x0F];
    }
    return out;
}

esp_err_t WiFiServerManager::api_ota_update_handler(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len > (int)OTA_MAX_IMAGE_BYTES) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_size\"}");
        return ESP_OK;
    }

    char hash_hdr[96] = {0};
    bool has_hash = httpd_req_get_hdr_value_str(req, "X-Firmware-Sha256", hash_hdr, sizeof(hash_hdr)) == ESP_OK;
    if (!has_hash) has_hash = httpd_req_get_hdr_value_str(req, "X-Checksum-Sha256", hash_hdr, sizeof(hash_hdr)) == ESP_OK;
    if (!has_hash) has_hash = httpd_req_get_hdr_value_str(req, "X-Firmware-Signature", hash_hdr, sizeof(hash_hdr)) == ESP_OK;
    if (!has_hash) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"missing_sha256\"}");
        return ESP_OK;
    }

    std::string expected_hex;
    if (!normalize_sha256_hex(hash_hdr, expected_hex)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_sha256_header\"}");
        return ESP_OK;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(nullptr);
    if (!update_partition) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (psa_crypto_init() != PSA_SUCCESS) {
        (void)esp_ota_abort(ota_handle);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    psa_hash_operation_t hash_op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hash_op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        (void)esp_ota_abort(ota_handle);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    std::string chunk;
    chunk.resize(4096);
    int remaining = req->content_len;
    while (remaining > 0) {
        const int to_read = std::min(remaining, (int)chunk.size());
        const int r = httpd_req_recv(req, chunk.data(), to_read);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            (void)esp_ota_abort(ota_handle);
            (void)psa_hash_abort(&hash_op);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        if (psa_hash_update(&hash_op, (const uint8_t*)chunk.data(), (size_t)r) != PSA_SUCCESS) {
            (void)esp_ota_abort(ota_handle);
            (void)psa_hash_abort(&hash_op);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, chunk.data(), (size_t)r);
        if (err != ESP_OK) {
            (void)esp_ota_abort(ota_handle);
            (void)psa_hash_abort(&hash_op);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        remaining -= r;
    }

    uint8_t digest[32] = {0};
    size_t digest_len = 0;
    if (psa_hash_finish(&hash_op, digest, sizeof(digest), &digest_len) != PSA_SUCCESS || digest_len != 32) {
        (void)esp_ota_abort(ota_handle);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    const std::string actual_hex = sha256_to_hex(digest);
    if (actual_hex != expected_hex) {
        (void)esp_ota_abort(ota_handle);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"sha256_mismatch\"}");
        return ESP_OK;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        (void)esp_ota_abort(ota_handle);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"rebooting\":true}");
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
    return ESP_OK;
}
