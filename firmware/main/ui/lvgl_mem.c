#include "ui/lvgl_mem.h"

#include <string.h>

#include "esp_heap_caps.h"

static const size_t s_tc_lvgl_psram_threshold = 2048;

static void *try_alloc_caps(size_t size, uint32_t caps) {
    if (size == 0) return NULL;
    return heap_caps_malloc(size, caps);
}

void *tc_lvgl_mem_alloc(size_t size) {
    const uint32_t caps_int = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t caps_psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    if (size >= s_tc_lvgl_psram_threshold) {
        void *p = try_alloc_caps(size, caps_psram);
        if (p) return p;
        return try_alloc_caps(size, caps_int);
    }

    void *p = try_alloc_caps(size, caps_int);
    if (p) return p;
    return try_alloc_caps(size, caps_psram);
}

void tc_lvgl_mem_free(void *ptr) {
    if (!ptr) return;
    heap_caps_free(ptr);
}

void *tc_lvgl_mem_realloc(void *ptr, size_t size) {
    if (!ptr) return tc_lvgl_mem_alloc(size);
    if (size == 0) {
        tc_lvgl_mem_free(ptr);
        return NULL;
    }
    const uint32_t caps_int = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t caps_psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    const uint32_t primary = (size >= s_tc_lvgl_psram_threshold) ? caps_psram : caps_int;
    const uint32_t secondary = (primary == caps_psram) ? caps_int : caps_psram;

    void *p = heap_caps_realloc(ptr, size, primary);
    if (p) return p;
    return heap_caps_realloc(ptr, size, secondary);
}
