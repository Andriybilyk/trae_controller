#include "kiln_config/fs_utils.h"

#include "kiln_config/config_store.h"

#include <cstdio>
#include <string>
#include <unistd.h>

std::string kiln_fs_read_text(const char *path, bool lock_fs) {
    if (!path || !path[0]) return {};
    SemaphoreHandle_t m = lock_fs ? kiln_config_fs_mutex() : nullptr;
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        if (m) xSemaphoreGiveRecursive(m);
        return {};
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        if (m) xSemaphoreGiveRecursive(m);
        return {};
    }
    std::string out;
    out.resize((size_t)size);
    const size_t r = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (m) xSemaphoreGiveRecursive(m);
    out.resize(r);
    return out;
}

bool kiln_fs_write_text(const char *path, const std::string &data, bool sync, bool lock_fs) {
    if (!path || !path[0]) return false;
    SemaphoreHandle_t m = lock_fs ? kiln_config_fs_mutex() : nullptr;
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    FILE *f = std::fopen(path, "wb");
    if (!f) {
        if (m) xSemaphoreGiveRecursive(m);
        return false;
    }
    const size_t w = std::fwrite(data.data(), 1, data.size(), f);
    if (sync) {
        std::fflush(f);
        const int fd = fileno(f);
        if (fd >= 0) (void)fsync(fd);
    }
    std::fclose(f);
    if (m) xSemaphoreGiveRecursive(m);
    return w == data.size();
}

bool kiln_fs_write_text_atomic(const char *path, const std::string &data, bool sync, bool lock_fs) {
    if (!path || !path[0]) return false;
    SemaphoreHandle_t m = lock_fs ? kiln_config_fs_mutex() : nullptr;
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    const std::string tmp = std::string(path) + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        if (m) xSemaphoreGiveRecursive(m);
        return false;
    }
    const size_t w = std::fwrite(data.data(), 1, data.size(), f);
    if (sync) {
        std::fflush(f);
        const int fd = fileno(f);
        if (fd >= 0) (void)fsync(fd);
    }
    std::fclose(f);
    if (w != data.size()) {
        (void)unlink(tmp.c_str());
        if (m) xSemaphoreGiveRecursive(m);
        return false;
    }
    if (rename(tmp.c_str(), path) != 0) {
        (void)unlink(path);
        if (rename(tmp.c_str(), path) != 0) {
            (void)unlink(tmp.c_str());
            if (m) xSemaphoreGiveRecursive(m);
            return false;
        }
    }
    if (m) xSemaphoreGiveRecursive(m);
    return true;
}

size_t kiln_fs_file_size(const char *path, bool lock_fs) {
    if (!path || !path[0]) return 0;
    SemaphoreHandle_t m = lock_fs ? kiln_config_fs_mutex() : nullptr;
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        if (m) xSemaphoreGiveRecursive(m);
        return 0;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fclose(f);
    if (m) xSemaphoreGiveRecursive(m);
    return size > 0 ? (size_t)size : 0;
}
