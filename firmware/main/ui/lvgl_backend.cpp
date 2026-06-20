#include "sdkconfig.h"

#if CONFIG_TC_UI_BACKEND_LVGL

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstdlib>

#include "drivers/display_driver.h"
#include "kiln_hal_heater/heater_hal.h"
#include "lvgl.h"
#include "ui.h"

static const char *TAG = "LVGL_BACKEND";

#if CONFIG_IDF_TARGET_ESP32P4
static constexpr int kUiWidth = 800;
static constexpr int kUiHeight = 480;
#else
static constexpr int kUiWidth = 480;
static constexpr int kUiHeight = 320;
#endif

static lv_display_t *s_disp = nullptr;
static lv_indev_t *s_indev = nullptr;
static uint16_t *s_draw_buf1 = nullptr;
static uint16_t *s_draw_buf2 = nullptr;
static bool s_backlight_enabled = false;
static bool s_blit_failed_logged = false;
static bool s_heater_display_ready_set = false;
static uint32_t s_flush_ok_count = 0;
static uint32_t s_flush_fail_count = 0;
static uint64_t s_last_flush_ms = 0;
static uint32_t s_buf_bytes = 0;
static int s_draw_buf_caps = 0;
static bool s_draw_buf_double = false;
static bool s_draw_buf_external = false;

extern "C" bool ui_lvgl_draw_buffer_memory_check(const char *phase) {
    const char *tag = (phase && phase[0]) ? phase : "unspecified";
    if (s_buf_bytes == 0) {
        ESP_LOGW(TAG, "DRAW_BUF_MEM phase=%s buf_bytes=0 ok=0", tag);
        return false;
    }

    int pool_caps = MALLOC_CAP_8BIT;
    const char *pool_name = "8BIT";
    if (s_draw_buf_external) {
        pool_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        pool_name = "PSRAM";
    } else if ((s_draw_buf_caps & MALLOC_CAP_DMA) != 0 && (s_draw_buf_caps & MALLOC_CAP_INTERNAL) != 0) {
        pool_caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
        pool_name = "DMA_INTERNAL";
    } else if ((s_draw_buf_caps & MALLOC_CAP_INTERNAL) != 0) {
        pool_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        pool_name = "INTERNAL";
    }

    const size_t pool_free = heap_caps_get_free_size(pool_caps);
    const size_t pool_largest = heap_caps_get_largest_free_block(pool_caps);
    const size_t required_largest = s_buf_bytes;
    const size_t required_free = s_draw_buf_double ? (size_t)s_buf_bytes * 2U : (size_t)s_buf_bytes;
    const size_t total_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t largest_8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#if CONFIG_IDF_TARGET_ESP32S3
    const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
#else
    const size_t dma_free = 0;
    const size_t dma_largest = 0;
#endif
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const bool ok = (pool_free >= required_free) && (pool_largest >= required_largest);

    ESP_LOGI(TAG,
             "DRAW_BUF_MEM phase=%s ok=%d pool=%s buf_bytes=%u buffers=%u need_free=%u need_largest=%u free=%u largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u free8=%u largest8=%u",
             tag,
             ok ? 1 : 0,
             pool_name,
             (unsigned)s_buf_bytes,
             s_draw_buf_double ? 2U : 1U,
             (unsigned)required_free,
             (unsigned)required_largest,
             (unsigned)pool_free,
             (unsigned)pool_largest,
             (unsigned)dma_free,
             (unsigned)dma_largest,
             (unsigned)psram_free,
             (unsigned)psram_largest,
             (unsigned)total_8bit,
             (unsigned)largest_8bit);
    return ok;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const int32_t x1 = area->x1;
    const int32_t y1 = area->y1;
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;

    const uint16_t *data = reinterpret_cast<const uint16_t *>(px_map);
    const bool ok = display_driver_blit_rgb565((int)x1, (int)y1, (int)w, (int)h, data);
    s_last_flush_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    if (ok) {
        s_flush_ok_count++;
    } else {
        s_flush_fail_count++;
    }
    if (!ok && !s_blit_failed_logged) {
        s_blit_failed_logged = true;
        ESP_LOGE(TAG, "display_driver_blit_rgb565 failed (area=%ld,%ld %ldx%ld)", (long)x1, (long)y1, (long)w, (long)h);
    }

    if (!s_backlight_enabled) {
        s_backlight_enabled = true;
        display_driver_show_backlight_after_first_frame(display_driver_get_backlight_percent());
    }
    if (!s_heater_display_ready_set) {
        s_heater_display_ready_set = true;
        heater_hal_set_display_ready(true);
    }

    lv_display_flush_ready(disp);
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t z = 0;
    const bool pressed = display_driver_touch_probe(&x, &y, &z);
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = (int16_t)x;
    data->point.y = (int16_t)y;

    static bool s_last_pressed = false;
    static uint64_t s_last_log_ms = 0;
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    if (pressed != s_last_pressed || (now_ms - s_last_log_ms) >= 1500) {
        s_last_pressed = pressed;
        s_last_log_ms = now_ms;
        ESP_LOGI(TAG, "DBG_LVGL_TOUCH pressed=%d x=%u y=%u z=%u", pressed ? 1 : 0,
                 (unsigned)x, (unsigned)y, (unsigned)z);
    }
}

static bool lvgl_hal_init() {
    display_driver_init();

    lv_init();

    s_disp = lv_display_create(kUiWidth, kUiHeight);
    if (!s_disp) return false;

    s_draw_buf1 = nullptr;
    s_draw_buf2 = nullptr;
    s_buf_bytes = 0;
    s_draw_buf_caps = 0;
    s_draw_buf_double = false;
    s_draw_buf_external = false;

    // Based on Espressif's spi_lcd_touch LVGL example:
    // - prefer partial rendering with relatively small buffers
    // - prefer INTERNAL+DMA buffers for stability and speed on ESP32-S3
#if CONFIG_IDF_TARGET_ESP32S3
    const uint32_t line_candidates[] = {40, 32, 24, 20, 16, 12, 10, 8, 6, 4};
#else
    const uint32_t line_candidates[] = {120, 80, 60, 48, 40, 32, 24, 16, 12, 8};
#endif
    bool used_psram = false;
    bool ok = false;
    bool double_buffer = false;

    const int caps_try_double[] = {
#if CONFIG_IDF_TARGET_ESP32S3
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
        MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM,
#else
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
        MALLOC_CAP_8BIT,
#endif
    };
    const int caps_try_single[] = {
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
        MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM,
        MALLOC_CAP_8BIT,
    };

    for (uint32_t want_double = 1; want_double <= 2 && !ok; want_double++) {
        const bool require_double = (want_double == 1);
        const int *caps_try = require_double ? caps_try_double : caps_try_single;
        const uint32_t caps_try_count = require_double ?
            (uint32_t)(sizeof(caps_try_double) / sizeof(caps_try_double[0])) :
            (uint32_t)(sizeof(caps_try_single) / sizeof(caps_try_single[0]));

        for (uint32_t cap_idx = 0; cap_idx < caps_try_count; cap_idx++) {
            const int caps = caps_try[cap_idx];
            for (uint32_t i = 0; i < (uint32_t)(sizeof(line_candidates) / sizeof(line_candidates[0])); i++) {
                const uint32_t lines = line_candidates[i];
                const uint32_t buf_px = (uint32_t)kUiWidth * lines;
                const uint32_t buf_bytes = buf_px * sizeof(uint16_t);

                uint16_t *b1 = (uint16_t *)heap_caps_malloc(buf_bytes, caps);
                if (!b1) continue;
                uint16_t *b2 = (uint16_t *)heap_caps_malloc(buf_bytes, caps);

                if (require_double && !b2) {
                    free(b1);
                    continue;
                }

#if CONFIG_IDF_TARGET_ESP32S3
                if (require_double && ((caps & MALLOC_CAP_DMA) != 0) && ((caps & MALLOC_CAP_INTERNAL) != 0)) {
                    const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
                    const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
                    if (dma_free < 60 * 1024 || dma_largest < 32 * 1024) {
                        if (b2) free(b2);
                        free(b1);
                        continue;
                    }
                }
#endif

                s_draw_buf1 = b1;
                s_draw_buf2 = b2;
                s_buf_bytes = buf_bytes;
                s_draw_buf_caps = caps;
                used_psram = esp_ptr_external_ram(b1) || (b2 && esp_ptr_external_ram(b2));
                s_draw_buf_external = used_psram;
                double_buffer = (b2 != nullptr);
                s_draw_buf_double = double_buffer;
                ok = true;
                break;
            }
        }
    }

    if (!ok || !s_draw_buf1 || s_buf_bytes == 0) return false;

    ESP_LOGI(TAG, "LVGL draw buffers: %u bytes each, mode=%s, lines=%u, buffers=%u", (unsigned)s_buf_bytes,
             used_psram ? "PSRAM" : "INTERNAL", (unsigned)(s_buf_bytes / (kUiWidth * sizeof(uint16_t))),
             double_buffer ? 2U : 1U);
#if CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGI(TAG, "LVGL DMA free after alloc: %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "LVGL DMA largest after alloc: %u",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
#endif
    if (!double_buffer) {
        ESP_LOGW(TAG, "LVGL using single draw buffer (performance reduced)");
    }

    (void)ui_lvgl_draw_buffer_memory_check("after_draw_buffer_alloc");


    lv_display_set_buffers(s_disp, s_draw_buf1, s_draw_buf2, s_buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);
    lv_display_set_default(s_disp);

    s_indev = lv_indev_create();
    if (!s_indev) return false;
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, lvgl_touch_read_cb);
    lv_indev_set_display(s_indev, s_disp);
    lv_timer_t *indev_timer = lv_indev_get_read_timer(s_indev);
    ESP_LOGI(TAG, "DBG_INDEV init indev=%p timer=%p paused=%d",
             (void *)s_indev,
             (void *)indev_timer,
             indev_timer ? (lv_timer_get_paused(indev_timer) ? 1 : 0) : -1);

    return true;
}

extern "C" void lvgl_ui_run() {
    ESP_LOGI(TAG, "LVGL UI starting (KilnPro mode)");

    if (!lvgl_hal_init()) {
        ESP_LOGE(TAG, "LVGL HAL init failed");
        vTaskDelete(nullptr);
        return;
    }

    {
        const esp_err_t err = esp_task_wdt_add(NULL);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "esp_task_wdt_add(UI) failed: %s", esp_err_to_name(err));
        }
    }

    ui_init();

    ESP_LOGI(TAG, "LVGL UI initialized, entering main loop");

    int64_t last_tick_us = esp_timer_get_time();
    uint64_t last_indev_dbg_ms = 0;
    while (true) {
        (void)esp_task_wdt_reset();
        const int64_t now_tick_us = esp_timer_get_time();
        int64_t dt_us = now_tick_us - last_tick_us;
        if (dt_us < 0) dt_us = 0;
        const uint32_t dt_ms = (uint32_t)(dt_us / 1000);
        if (dt_ms > 0) {
            lv_tick_inc(dt_ms);
            last_tick_us += (int64_t)dt_ms * 1000;
        }

        uint32_t sleep_ms = lv_timer_handler_run_in_period(20);
        if (sleep_ms < 5) sleep_ms = 5;
        if (sleep_ms > 20) sleep_ms = 20;

        (void)esp_task_wdt_reset();
        ui_tick();

        const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        if ((now_ms - last_indev_dbg_ms) >= 2000) {
            last_indev_dbg_ms = now_ms;
            lv_timer_t *indev_timer = s_indev ? lv_indev_get_read_timer(s_indev) : nullptr;
            ESP_LOGI(TAG, "DBG_INDEV loop indev=%p timer=%p paused=%d",
                     (void *)s_indev,
                     (void *)indev_timer,
                     indev_timer ? (lv_timer_get_paused(indev_timer) ? 1 : 0) : -1);
        }

        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}

#endif // CONFIG_TC_UI_BACKEND_LVGL
