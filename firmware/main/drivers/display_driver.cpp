#include "drivers/display_driver.h"

#include "config/board_profile.h"
#include "kiln_config/config.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"

#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "driver/i2c_master.h"
#endif

static const char *TAG = "DISPLAY";

#if CONFIG_IDF_TARGET_ESP32P4
static esp_lcd_dsi_bus_handle_t s_p4_dsi_bus = nullptr;
static esp_lcd_panel_io_handle_t s_p4_dbi_io = nullptr;
static esp_lcd_panel_handle_t s_p4_panel = nullptr;
static i2c_master_bus_handle_t s_p4_touch_i2c_bus = nullptr;
static std::array<uint16_t *, 3> s_p4_framebuffers{};
static int s_p4_framebuffer_count = 0;
static int s_p4_framebuffer_index = 0;
static uint16_t *s_p4_pending_fb = nullptr;
static int s_p4_pending_index = -1;
static size_t s_p4_framebuffer_pixels = 0;
static bool s_p4_display_ready = false;
static bool s_p4_backlight_pwm_ready = false;
static bool s_p4_backlight_runtime_locked = false;
static bool s_p4_backlight_deferred_until_first_frame = false;
static constexpr bool kP4RenderTrace = false;
static bool s_p4_use_framebuffer = false;
static SemaphoreHandle_t s_p4_refresh_sem = nullptr;
static SemaphoreHandle_t s_p4_render_sem = nullptr;
static SemaphoreHandle_t s_p4_submit_mutex = nullptr;
static TaskHandle_t s_p4_submit_task = nullptr;
static std::array<uint16_t *, 3> s_p4_submit_bufs{};
static uint16_t *s_p4_submit_pending_buf = nullptr;
static uint16_t *s_p4_submit_inflight_buf = nullptr;
static size_t s_p4_submit_pending_px = 0;
static size_t s_p4_submit_buf_capacity_px = 0;
static int s_p4_submit_x1 = 0;
static int s_p4_submit_y1 = 0;
static int s_p4_submit_x2 = 0;
static int s_p4_submit_y2 = 0;
static bool s_p4_submit_pending = false;
static TickType_t s_p4_last_vsync_warn = 0;
static constexpr int P4_PANEL_PHYS_W = 480;
static constexpr int P4_PANEL_PHYS_H = 800;
static constexpr int P4_UI_LOGICAL_W = 800;
static constexpr int P4_UI_LOGICAL_H = 480;
static constexpr int P4_RENDER_W = 800;
static constexpr int P4_RENDER_H = 480;
static constexpr int P4_LANDSCAPE_W = 800;
static constexpr int P4_LANDSCAPE_H = 480;
static constexpr gpio_num_t P4_BACKLIGHT_GPIO = GPIO_NUM_23;
static constexpr gpio_num_t P4_BACKLIGHT_GPIO_ALT = GPIO_NUM_NC;
static constexpr gpio_num_t P4_LCD_RESET_GPIO = GPIO_NUM_5;
static constexpr gpio_num_t P4_TOUCH_SDA_GPIO = GPIO_NUM_7;
static constexpr gpio_num_t P4_TOUCH_SCL_GPIO = GPIO_NUM_8;
static constexpr std::array<uint8_t, 2> P4_GT911_ADDR_CANDIDATES = {0x5D, 0x14};
static constexpr uint8_t kP4BacklightDefault = 100;

// LEDC PWM fallback (if no I2C backlight driver is present).
static constexpr ledc_mode_t kP4BacklightMode = LEDC_LOW_SPEED_MODE;
// Match the known-good reference configuration for this module: TIMER_1 @ 5kHz, 10-bit.
// BUZZER uses TIMER_1/CHANNEL_1; FAN uses TIMER_0/CHANNEL_0 on the legacy board, but on NewP4 FAN is invalid/skipped.
static constexpr ledc_timer_t kP4BacklightTimer = LEDC_TIMER_1;
static constexpr ledc_channel_t kP4BacklightChannel = LEDC_CHANNEL_2;
static constexpr ledc_timer_bit_t kP4BacklightDutyRes = LEDC_TIMER_10_BIT; // 0..1023
static constexpr uint32_t kP4BacklightPwmHz = 5000;
static constexpr bool kP4BacklightUsePwm = true;
static constexpr bool kP4ForceBacklightDc = false;
static constexpr bool kP4BacklightActiveLow = false;
static constexpr bool kP4BacklightSendDcs51 = true;
static constexpr bool kP4BacklightTestPulse = false;
static constexpr bool kP4BacklightTestUseAlt = false; // false = drive GPIO23, true = drive GPIO26
static constexpr bool kP4UseFramebuffers = true;
static bool s_p4_blit_fence_enabled = true;

static uint16_t *p4_alloc_submit_buf(size_t px_capacity) {
    const size_t bytes = px_capacity * sizeof(uint16_t);
    uint16_t *p = (uint16_t *)heap_caps_aligned_calloc(64, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
    return (uint16_t *)heap_caps_aligned_calloc(64, 1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void p4_submit_worker_task(void *) {
    uint16_t *local_buf = nullptr;
    size_t local_px = 0;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_p4_submit_mutex) {
            continue;
        }

        xSemaphoreTake(s_p4_submit_mutex, portMAX_DELAY);
        if (s_p4_submit_pending) {
            x1 = s_p4_submit_x1;
            y1 = s_p4_submit_y1;
            x2 = s_p4_submit_x2;
            y2 = s_p4_submit_y2;
            local_buf = s_p4_submit_pending_buf;
            local_px = s_p4_submit_pending_px;
            s_p4_submit_pending_buf = nullptr;
            s_p4_submit_pending_px = 0;
            s_p4_submit_inflight_buf = local_buf;
            s_p4_submit_pending = false;
        }
        xSemaphoreGive(s_p4_submit_mutex);

        if (!local_buf || local_px == 0 || !s_p4_display_ready || s_p4_panel == nullptr) {
            continue;
        }

        if (s_p4_blit_fence_enabled && s_p4_render_sem) {
            if (xSemaphoreTake(s_p4_render_sem, pdMS_TO_TICKS(120)) != pdTRUE) {
                ESP_LOGW(TAG, "P4 submit worker: timeout waiting render fence");
                xSemaphoreTake(s_p4_submit_mutex, portMAX_DELAY);
                if (s_p4_submit_inflight_buf == local_buf) {
                    s_p4_submit_inflight_buf = nullptr;
                }
                xSemaphoreGive(s_p4_submit_mutex);
                local_buf = nullptr;
                local_px = 0;
                continue;
            }
        }

        const esp_err_t draw_ok = esp_lcd_panel_draw_bitmap(
            s_p4_panel,
            x1,
            y1,
            x2 + 1,
            y2 + 1,
            local_buf);
        if (draw_ok != ESP_OK && s_p4_render_sem) {
            // Release fence on failure path to avoid deadlock.
            (void)xSemaphoreGive(s_p4_render_sem);
        }

        xSemaphoreTake(s_p4_submit_mutex, portMAX_DELAY);
        if (s_p4_submit_inflight_buf == local_buf) {
            s_p4_submit_inflight_buf = nullptr;
        }
        xSemaphoreGive(s_p4_submit_mutex);
        local_buf = nullptr;
        local_px = 0;
    }
}

static void p4_i2c_peek_device(i2c_master_bus_handle_t bus, uint8_t addr) {
    if (!bus) return;

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr;
    dev_cfg.scl_speed_hz = 400000;

    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK || !dev) return;

    // Best-effort register peeks. If device doesn't support this protocol, reads will fail quickly.
    for (uint8_t reg = 0; reg < 0x08; ++reg) {
        uint8_t val = 0;
        const esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, &val, 1, 10);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "P4 I2C peek addr=0x%02X reg=0x%02X -> 0x%02X", (int)addr, (int)reg, (int)val);
        }
    }
}

static void p4_i2c_identify_known_devices(i2c_master_bus_handle_t bus) {
    // Addresses observed on this module: 0x14 0x18 0x36 0x5D (GT911).
    // We peek a few regs on 0x14/0x18/0x36 to identify what they are.
    p4_i2c_peek_device(bus, 0x14);
    p4_i2c_peek_device(bus, 0x18);
    p4_i2c_peek_device(bus, 0x36);
}

static void p4_i2c_scan(i2c_master_bus_handle_t bus) {
    if (!bus) return;
    char found[256] = {0};
    size_t pos = 0;
    for (int addr = 0x03; addr <= 0x77; ++addr) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            const int n = snprintf(found + pos, sizeof(found) - pos, " 0x%02X", addr);
            if (n > 0) pos = std::min(pos + (size_t)n, sizeof(found) - 1);
        }
    }
    if (pos > 0) {
        ESP_LOGI(TAG, "P4 I2C scan (GPIO7/8):%s", found);
    } else {
        ESP_LOGW(TAG, "P4 I2C scan (GPIO7/8): no devices found");
    }
}

// JC4880P443 (ST7701) init sequence from known-good bring-up.
static const st7701_lcd_init_cmd_t s_jc4880p443_init_cmds[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x10, 0x08}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},
    {0xB0, (uint8_t[]){0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}, 16, 0},
    {0xB1, (uint8_t[]){0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x5D}, 1, 0},
    {0xB1, (uint8_t[]){0x58}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x4E}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xB9, (uint8_t[]){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t[]){0x03}, 1, 0},
    {0xBC, (uint8_t[]){0x00}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t[]){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t[]){0x08, 0x01}, 2, 0},
    {0xED, (uint8_t[]){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 20},
};

static void p4_backlight_apply_percent(uint8_t percent) {
    if (!GPIO_IS_VALID_OUTPUT_GPIO(P4_BACKLIGHT_GPIO)) return;
    percent = (uint8_t)std::clamp<int>(percent, 0, 100);
    if (kP4BacklightSendDcs51 && s_p4_dbi_io) {
        const uint8_t dcs = (uint8_t)(((uint32_t)percent * 255u) / 100u);
        (void)esp_lcd_panel_io_tx_param(s_p4_dbi_io, 0x51, (uint8_t[]){dcs}, 1);
    }
    ESP_LOGI(TAG, "P4 BL apply=%u (dc=%d pwm_ready=%d lock=%d)",
             (unsigned)percent,
             (int)kP4ForceBacklightDc,
             (int)s_p4_backlight_pwm_ready,
             (int)s_p4_backlight_runtime_locked);
    ESP_LOGI(TAG, "BACKLIGHT apply=%u mode=%s",
             (unsigned)percent,
             kP4ForceBacklightDc ? "dc" : "pwm");
    if (kP4BacklightTestPulse) {
        const int level = (percent > 0) ? (kP4BacklightActiveLow ? 0 : 1) : (kP4BacklightActiveLow ? 1 : 0);
        if (kP4BacklightTestUseAlt) {
            gpio_set_level(P4_BACKLIGHT_GPIO, kP4BacklightActiveLow ? 0 : 1);
            if (GPIO_IS_VALID_OUTPUT_GPIO(P4_BACKLIGHT_GPIO_ALT)) {
                gpio_set_level(P4_BACKLIGHT_GPIO_ALT, level);
            }
        } else {
            gpio_set_level(P4_BACKLIGHT_GPIO, level);
            if (GPIO_IS_VALID_OUTPUT_GPIO(P4_BACKLIGHT_GPIO_ALT)) {
                gpio_set_level(P4_BACKLIGHT_GPIO_ALT, kP4BacklightActiveLow ? 0 : 1);
            }
        }
        return;
    }
    if (kP4ForceBacklightDc || !kP4BacklightUsePwm) {
        const int level = (percent > 0) ? (kP4BacklightActiveLow ? 0 : 1) : (kP4BacklightActiveLow ? 1 : 0);
        gpio_set_level(P4_BACKLIGHT_GPIO, level);
        if (GPIO_IS_VALID_OUTPUT_GPIO(P4_BACKLIGHT_GPIO_ALT)) {
            // Keep ALT low/off to avoid unintended drive if connected elsewhere
            gpio_set_level(P4_BACKLIGHT_GPIO_ALT, kP4BacklightActiveLow ? 1 : 0);
        }
        return;
    }

    if (s_p4_backlight_pwm_ready) {
        const uint32_t max_duty = (1u << (uint32_t)kP4BacklightDutyRes) - 1u;
        const uint32_t duty_raw = ((uint32_t)std::clamp<int>(percent, 0, 100) * max_duty) / 100u;
        const uint32_t duty = kP4BacklightActiveLow ? (max_duty - duty_raw) : duty_raw;
        (void)ledc_set_duty(kP4BacklightMode, kP4BacklightChannel, duty);
        (void)ledc_update_duty(kP4BacklightMode, kP4BacklightChannel);
    } else {
        const int level = (percent > 0) ? (kP4BacklightActiveLow ? 0 : 1) : (kP4BacklightActiveLow ? 1 : 0);
        gpio_set_level(P4_BACKLIGHT_GPIO, level);
        if (GPIO_IS_VALID_OUTPUT_GPIO(P4_BACKLIGHT_GPIO_ALT)) {
            gpio_set_level(P4_BACKLIGHT_GPIO_ALT, level);
        }
    }
}

static void p4_backlight_init(void) {
    if (!GPIO_IS_VALID_OUTPUT_GPIO(P4_BACKLIGHT_GPIO)) return;
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << (uint32_t)P4_BACKLIGHT_GPIO);
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    (void)gpio_config(&io);
    gpio_set_drive_capability(P4_BACKLIGHT_GPIO, GPIO_DRIVE_CAP_3);

    // Keep the alternate candidate pin OFF unless explicitly testing it.
    if (GPIO_IS_VALID_OUTPUT_GPIO(P4_BACKLIGHT_GPIO_ALT)) {
        gpio_config_t io2 = {};
        io2.pin_bit_mask = (1ULL << (((uint32_t)P4_BACKLIGHT_GPIO_ALT) & 63U));
        io2.mode = GPIO_MODE_OUTPUT;
        io2.pull_up_en = GPIO_PULLUP_DISABLE;
        io2.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io2.intr_type = GPIO_INTR_DISABLE;
        (void)gpio_config(&io2);
        gpio_set_drive_capability(P4_BACKLIGHT_GPIO_ALT, GPIO_DRIVE_CAP_3);
        gpio_set_level(P4_BACKLIGHT_GPIO_ALT, kP4BacklightActiveLow ? 1 : 0);
    }

    if (kP4BacklightTestPulse) {
        s_p4_backlight_pwm_ready = false;
        gpio_set_level(P4_BACKLIGHT_GPIO, kP4BacklightActiveLow ? 1 : 0);
        return;
    }
    if (kP4ForceBacklightDc || !kP4BacklightUsePwm) {
        s_p4_backlight_pwm_ready = false;
        gpio_set_level(P4_BACKLIGHT_GPIO, kP4BacklightActiveLow ? 1 : 0);
        return;
    }

    // PWM mode (reference config).
    ledc_timer_config_t timer = {};
    timer.speed_mode = kP4BacklightMode;
    timer.timer_num = kP4BacklightTimer;
    timer.duty_resolution = kP4BacklightDutyRes;
    timer.freq_hz = kP4BacklightPwmHz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK) {
        s_p4_backlight_pwm_ready = false;
        gpio_set_level(P4_BACKLIGHT_GPIO, kP4BacklightActiveLow ? 1 : 0);
        return;
    }

    ledc_channel_config_t ch = {};
    ch.speed_mode = kP4BacklightMode;
    ch.channel = kP4BacklightChannel;
    ch.timer_sel = kP4BacklightTimer;
    ch.gpio_num = (int)P4_BACKLIGHT_GPIO;
    ch.duty = 0;
    ch.hpoint = 0;
    if (ledc_channel_config(&ch) != ESP_OK) {
        s_p4_backlight_pwm_ready = false;
        gpio_set_level(P4_BACKLIGHT_GPIO, kP4BacklightActiveLow ? 1 : 0);
        return;
    }
    s_p4_backlight_pwm_ready = true;
    ESP_LOGI(TAG, "P4 backlight PWM ready (timer=%d ch=%d target=%uHz actual=%uHz)",
             (int)kP4BacklightTimer,
             (int)kP4BacklightChannel,
             (unsigned)kP4BacklightPwmHz,
             (unsigned)ledc_get_freq(kP4BacklightMode, kP4BacklightTimer));
}
#endif

static constexpr spi_host_device_t LCD_HOST = SPI2_HOST;
static constexpr spi_host_device_t TOUCH_HOST = SPI3_HOST;

static spi_device_handle_t s_lcd_spi = nullptr;
static spi_host_device_t s_touch_host = LCD_HOST;

// ST7796 expects RGB565 pixels MSB-first. Slint's software renderer produces native-endian u16
// pixels (little-endian on ESP32-S3), so we byte-swap before pushing to the panel.
static bool s_lcd_swap_bytes = true;
static volatile bool s_lcd_flush_in_progress = false;

struct LcdFlushGuard {
    LcdFlushGuard() { s_lcd_flush_in_progress = true; }
    ~LcdFlushGuard() { s_lcd_flush_in_progress = false; }
};

static constexpr int DISPLAY_WIDTH = 480;  // LVGL landscape
static constexpr int DISPLAY_HEIGHT = 320; // LVGL landscape
static constexpr int LCD_X_OFFSET = 0;
static constexpr int LCD_Y_OFFSET = 0;
static constexpr size_t LCD_SPI_CHUNK_BYTES =
    (size_t)DISPLAY_WIDTH * (size_t)((TFT_SPI_DMA_LINES > 0) ? TFT_SPI_DMA_LINES : 1) * 2U;

static bool s_mirror_x = false;
static bool s_mirror_y = false;
static int s_lcd_x_offset = 0;
static int s_lcd_y_offset = 0;
static uint8_t s_backlight_percent = 100;
static bool s_backlight_inited = false;

static constexpr ledc_mode_t kBacklightMode = LEDC_LOW_SPEED_MODE;
static constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_2;
static constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_2;
static constexpr ledc_timer_bit_t kBacklightDutyRes = LEDC_TIMER_10_BIT; // 0..1023
static constexpr uint32_t kBacklightPwmHz = 5000;
static constexpr uint16_t kTouchMinZ1 = 80;
static constexpr uint8_t kTouchDebounceSamples = 2;
static constexpr int kTouchSpikeMax = 80;
static constexpr uint16_t kTouchSpikeWeakZ1 = 120;
static constexpr int kTouchSampleSpanMax = 50;
static constexpr uint16_t kTouchStableZ1 = 160;

static uint8_t s_touch_press_streak = 0;
static uint8_t s_touch_release_streak = 0;
static bool s_touch_debounced = false;
static bool s_backlight_pwm_enabled = false;

static int s_touch_spi_mode = 0;
static int s_touch_spi_hz = TOUCH_SPI_CLOCK_HZ;

static bool s_touch_swap_xy = false;
static bool s_touch_mirror_x = false;
static bool s_touch_mirror_y = false;

static bool s_touch_cal_enabled = false;
static uint16_t s_touch_cal_left = 0;
static uint16_t s_touch_cal_right = DISPLAY_WIDTH - 1;
static uint16_t s_touch_cal_top = 0;
static uint16_t s_touch_cal_bottom = DISPLAY_HEIGHT - 1;

static bool s_touch_affine_enabled = false;
static float s_touch_affine_a = 1.0f;
static float s_touch_affine_b = 0.0f;
static float s_touch_affine_c = 0.0f;
static float s_touch_affine_d = 0.0f;
static float s_touch_affine_e = 1.0f;
static float s_touch_affine_f = 0.0f;

static bool s_touch_grid_enabled = false;
static std::array<float, 9> s_touch_grid_dx{};
static std::array<float, 9> s_touch_grid_dy{};

// Optional quadratic correction for Y to compensate nonlinearity.
// y' = a*y^2 + b*y + c
static bool s_touch_quad_enabled = false;
static float s_touch_quad_a = 0.0044125721f;
static float s_touch_quad_b = -0.65727115f;
static float s_touch_quad_c = 60.641743f;

static gpio_num_t s_touch_sclk = TOUCH_SCLK;
static gpio_num_t s_touch_mosi = TOUCH_MOSI;
static gpio_num_t s_touch_miso = TOUCH_MISO;

static esp_lcd_panel_io_handle_t s_touch_io = nullptr;
static esp_lcd_touch_handle_t s_touch = nullptr;
static SemaphoreHandle_t s_touch_mutex = nullptr;

static volatile uint16_t s_touch_last_raw_x = 0;
static volatile uint16_t s_touch_last_raw_y = 0;
static volatile uint16_t s_touch_last_z1 = 0;
static volatile bool s_touch_last_pressed = false;
static volatile uint32_t s_touch_read_cb_count = 0;
static volatile uint8_t s_touch_last_z1_bytes[3] = {0};
static volatile uint8_t s_touch_last_x_bytes[3] = {0};
static volatile uint8_t s_touch_last_y_bytes[3] = {0};

static void save_mirror_to_nvs();

#if CONFIG_IDF_TARGET_ESP32P4
static bool p4_on_refresh_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *, void *) {
    BaseType_t high_task_woken = pdFALSE;
    if (s_p4_refresh_sem) {
        xSemaphoreGiveFromISR(s_p4_refresh_sem, &high_task_woken);
    }
    if (s_p4_render_sem) {
        xSemaphoreGiveFromISR(s_p4_render_sem, &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

static void p4_fill_black_sync(void) {
    if (!s_p4_panel) return;
    const int w = P4_PANEL_PHYS_W;
    const int h = P4_PANEL_PHYS_H;
    const int strip = 80;
    uint16_t *black_buf = (uint16_t *)heap_caps_aligned_calloc(
        64,
        1,
        (size_t)w * (size_t)strip * sizeof(uint16_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!black_buf) return;
    memset(black_buf, 0, (size_t)w * (size_t)strip * sizeof(uint16_t));
    for (int y = 0; y < h; y += strip) {
        const int y_end = (y + strip > h) ? h : (y + strip);
        (void)esp_lcd_panel_draw_bitmap(s_p4_panel, 0, y, w, y_end, black_buf);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    for (int y = 0; y < h; y += strip) {
        const int y_end = (y + strip > h) ? h : (y + strip);
        (void)esp_lcd_panel_draw_bitmap(s_p4_panel, 0, y, w, y_end, black_buf);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    heap_caps_free(black_buf);
}

static esp_err_t p4_display_init(void) {
    // Power MIPI DSI PHY (required on ESP32-P4).
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_ldo_channel_handle_t ldo_mipi_phy = nullptr;
    esp_ldo_channel_config_t ldo_cfg = {};
    ldo_cfg.chan_id = 3;
    ldo_cfg.voltage_mv = 2500;
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy), TAG, "p4 ldo");
    vTaskDelay(pdMS_TO_TICKS(100));

    const int panel_w = P4_PANEL_PHYS_W;
    const int panel_h = P4_PANEL_PHYS_H;

    esp_lcd_dsi_bus_config_t bus_config = {};
    bus_config.bus_id = 0;
    bus_config.num_data_lanes = 2;
    bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    // Keep lane rate conservative to improve signal margin on this board/cable set.
    bus_config.lane_bit_rate_mbps = 500;
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &s_p4_dsi_bus), TAG, "p4 dsi bus");

    esp_lcd_dbi_io_config_t dbi_config = {};
    dbi_config.virtual_channel = 0;
    dbi_config.lcd_cmd_bits = 8;
    dbi_config.lcd_param_bits = 8;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_p4_dsi_bus, &dbi_config, &s_p4_dbi_io), TAG, "p4 dbi io");

    // Setup ST7701 DPI panel with dual framebuffers.
    esp_lcd_dpi_panel_config_t dpi_config = {};
    dpi_config.virtual_channel = 0;
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    // Reference-stable profile (JC4880P4x / ST7701 MIPI-DPI):
    // higher DPI clock with relaxed porches improves scan stability on this panel.
    dpi_config.dpi_clock_freq_mhz = 34.0f;
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.out_color_format = LCD_COLOR_FMT_RGB565;
    // Keep double buffering to reduce PSRAM traffic bursts while preserving stable DSI+DPI scanout.
    dpi_config.num_fbs = 2;
    dpi_config.video_timing.h_size = (uint32_t)panel_w;
    dpi_config.video_timing.v_size = (uint32_t)panel_h;
    dpi_config.video_timing.hsync_pulse_width = 12;
    dpi_config.video_timing.hsync_back_porch = 42;
    dpi_config.video_timing.hsync_front_porch = 42;
    dpi_config.video_timing.vsync_pulse_width = 2;
    dpi_config.video_timing.vsync_back_porch = 8;
    dpi_config.video_timing.vsync_front_porch = 166;
    dpi_config.flags.disable_lp = 0;

    st7701_vendor_config_t st_cfg = {};
    st_cfg.init_cmds = s_jc4880p443_init_cmds;
    st_cfg.init_cmds_size = sizeof(s_jc4880p443_init_cmds) / sizeof(s_jc4880p443_init_cmds[0]);
    st_cfg.flags.use_mipi_interface = 1;
    st_cfg.mipi_config.dsi_bus = s_p4_dsi_bus;
    st_cfg.mipi_config.dpi_config = &dpi_config;

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = P4_LCD_RESET_GPIO;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.vendor_config = &st_cfg;

    esp_err_t err = esp_lcd_new_panel_st7701(s_p4_dbi_io, &panel_cfg, &s_p4_panel);
    ESP_RETURN_ON_ERROR(err, TAG, "p4 panel create");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_p4_panel), TAG, "p4 panel reset");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_p4_panel), TAG, "p4 panel init");
    // Some JC4880P4x modules require explicit display brightness/control commands after init.
    // If omitted, panel can stay black while backlight is on.
    (void)esp_lcd_panel_io_tx_param(s_p4_dbi_io, 0x53, (uint8_t[]){0x24}, 1); // BCTRL/Display control
    (void)esp_lcd_panel_io_tx_param(s_p4_dbi_io, 0x51, (uint8_t[]){0xFF}, 1); // Max panel brightness
    (void)esp_lcd_panel_io_tx_param(s_p4_dbi_io, 0x55, (uint8_t[]){0x00}, 1); // Disable CABC
    // Avoid visible BL "blink" during init; keep BL stable and just clear the frame.
    p4_fill_black_sync();
    (void)esp_lcd_panel_disp_on_off(s_p4_panel, true);
    s_p4_framebuffer_pixels = 0;
    s_p4_use_framebuffer = false;
    s_p4_framebuffer_count = 0;
    s_p4_framebuffer_index = 0;
    s_p4_framebuffers = {};

    if (kP4UseFramebuffers) {
        void *fb0 = nullptr;
        void *fb1 = nullptr;
        if (esp_lcd_dpi_panel_get_frame_buffer(s_p4_panel, 2, &fb0, &fb1) == ESP_OK && fb0 && fb1) {
            s_p4_framebuffers[0] = static_cast<uint16_t *>(fb0);
            s_p4_framebuffers[1] = static_cast<uint16_t *>(fb1);
            s_p4_framebuffer_count = 2;
            s_p4_framebuffer_index = 0;
            s_p4_framebuffer_pixels = (size_t)P4_PANEL_PHYS_W * (size_t)P4_PANEL_PHYS_H;
            s_p4_use_framebuffer = true;
            memset(s_p4_framebuffers[0], 0, s_p4_framebuffer_pixels * sizeof(uint16_t));
            memset(s_p4_framebuffers[1], 0, s_p4_framebuffer_pixels * sizeof(uint16_t));
            (void)esp_lcd_panel_draw_bitmap(s_p4_panel, 0, 0, P4_PANEL_PHYS_W, P4_PANEL_PHYS_H, s_p4_framebuffers[0]);
        }
    }

    if (!s_p4_refresh_sem) {
        s_p4_refresh_sem = xSemaphoreCreateBinary();
    }
    if (!s_p4_render_sem) {
        s_p4_render_sem = xSemaphoreCreateBinary();
    }
    if (s_p4_refresh_sem) {
        xSemaphoreGive(s_p4_refresh_sem);
        esp_lcd_dpi_panel_event_callbacks_t cbs = {};
        // For MIPI DPI, use refresh completion as frame fence.
        // on_color_trans_done only means the draw buffer is copied/consumed,
        // not that the panel finished scanning out the frame.
        cbs.on_refresh_done = p4_on_refresh_done;
        (void)esp_lcd_dpi_panel_register_event_callbacks(s_p4_panel, &cbs, nullptr);
    }
    if (s_p4_render_sem) {
        xSemaphoreGive(s_p4_render_sem);
    }
    if (!s_p4_submit_mutex) {
        s_p4_submit_mutex = xSemaphoreCreateMutex();
    }
    if (!s_p4_submit_task) {
        xTaskCreate(p4_submit_worker_task, "p4_submit", 4096, nullptr, 6, &s_p4_submit_task);
    }
    if (s_p4_submit_buf_capacity_px == 0) {
        s_p4_submit_buf_capacity_px = (size_t)P4_PANEL_PHYS_W * (size_t)P4_PANEL_PHYS_H;
    }
    if (!s_p4_submit_bufs[0]) {
        for (auto &b : s_p4_submit_bufs) {
            b = p4_alloc_submit_buf(s_p4_submit_buf_capacity_px);
        }
    }
    // Keep panel backlight strictly OFF until Slint confirms first stable frame.
    // Never apply runtime brightness during panel init path.
    p4_backlight_apply_percent(0);

    s_p4_display_ready = true;

    ESP_LOGI(TAG, "P4 MIPI panel ready (physical): %dx%d", panel_w, panel_h);
    return ESP_OK;
}
#endif

static void load_mirror_from_nvs() {
    nvs_handle_t h;
    if (nvs_open("display", NVS_READONLY, &h) != ESP_OK) return;

    bool fix_bl = false;
    uint8_t mx = 0, my = 0;
    if (nvs_get_u8(h, "mx", &mx) == ESP_OK) s_mirror_x = (mx != 0);
    if (nvs_get_u8(h, "my", &my) == ESP_OK) s_mirror_y = (my != 0);
    int32_t xoff = 0;
    int32_t yoff = 0;
    if (nvs_get_i32(h, "xoff", &xoff) == ESP_OK) s_lcd_x_offset = (int)xoff;
    if (nvs_get_i32(h, "yoff", &yoff) == ESP_OK) s_lcd_y_offset = (int)yoff;
    uint8_t bl = 100;
    if (nvs_get_u8(h, "bl", &bl) == ESP_OK) {
        s_backlight_percent = (uint8_t)std::clamp<int>(bl, 0, 100);
        if (s_backlight_percent == 0) {
            s_backlight_percent = 75;
            fix_bl = true;
        }
    }
    s_lcd_x_offset = std::clamp(s_lcd_x_offset, -40, 40);
    s_lcd_y_offset = std::clamp(s_lcd_y_offset, -40, 40);
    nvs_close(h);

    if (fix_bl) {
        nvs_handle_t hw;
        if (nvs_open("display", NVS_READWRITE, &hw) == ESP_OK) {
            (void)nvs_set_u8(hw, "bl", s_backlight_percent);
            (void)nvs_commit(hw);
            nvs_close(hw);
        }
    }
}

#if !CONFIG_IDF_TARGET_ESP32P4
static void normalize_s3_default_orientation(void) {
    if (!s_mirror_x && !s_mirror_y) return;

    ESP_LOGW(TAG,
             "S3 orientation override: persisted mirror detected mx=%d my=%d, restoring defaults",
             (int)s_mirror_x,
             (int)s_mirror_y);
    s_mirror_x = false;
    s_mirror_y = false;
    save_mirror_to_nvs();
}
#endif

static void save_mirror_to_nvs() {
#if CONFIG_IDF_TARGET_ESP32P4
    // On ESP32-P4, synchronous NVS writes cause visible display flicker.
    // We defer mirror settings save as they are not performance critical.
    struct MirrorParams { bool mx, my; int xoff, yoff; };
    MirrorParams *p = (MirrorParams*)malloc(sizeof(MirrorParams));
    if (p) {
        p->mx = s_mirror_x; p->my = s_mirror_y;
        p->xoff = s_lcd_x_offset; p->yoff = s_lcd_y_offset;
        xTaskCreate([](void* arg){
            MirrorParams *p = (MirrorParams*)arg;
            nvs_handle_t h;
            if (nvs_open("display", NVS_READWRITE, &h) == ESP_OK) {
                (void)nvs_set_u8(h, "mx", p->mx ? 1 : 0);
                (void)nvs_set_u8(h, "my", p->my ? 1 : 0);
                (void)nvs_set_i32(h, "xoff", (int32_t)p->xoff);
                (void)nvs_set_i32(h, "yoff", (int32_t)p->yoff);
                (void)nvs_commit(h);
                nvs_close(h);
            }
            free(p);
            vTaskDelete(NULL);
        }, "mirror_save", 2048, p, 2, NULL);
    }
#else
    nvs_handle_t h;
    if (nvs_open("display", NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u8(h, "mx", s_mirror_x ? 1 : 0);
    (void)nvs_set_u8(h, "my", s_mirror_y ? 1 : 0);
    (void)nvs_set_i32(h, "xoff", (int32_t)s_lcd_x_offset);
    (void)nvs_set_i32(h, "yoff", (int32_t)s_lcd_y_offset);
    (void)nvs_commit(h);
    nvs_close(h);
#endif
}

static void save_backlight_to_nvs() {
#if CONFIG_IDF_TARGET_ESP32P4
    // Defer backlight save to avoid flicker during slider adjustment.
    static uint32_t s_pending_bl = 0xFFFFFFFF;
#if CONFIG_FREERTOS_USE_TIMERS
    static TimerHandle_t s_bl_timer = nullptr;

    s_pending_bl = s_backlight_percent;
    if (s_bl_timer == nullptr) {
        s_bl_timer = xTimerCreate("bl_save", pdMS_TO_TICKS(1000), pdFALSE, nullptr, [](TimerHandle_t){
            if (s_pending_bl <= 100) {
                nvs_handle_t h;
                if (nvs_open("display", NVS_READWRITE, &h) == ESP_OK) {
                    (void)nvs_set_u8(h, "bl", (uint8_t)s_pending_bl);
                    (void)nvs_commit(h);
                    nvs_close(h);
                }
            }
        });
    }
    if (s_bl_timer) xTimerReset(s_bl_timer, 0);
#else
    s_pending_bl = s_backlight_percent;
    if (s_pending_bl <= 100) {
        nvs_handle_t h;
        if (nvs_open("display", NVS_READWRITE, &h) == ESP_OK) {
            (void)nvs_set_u8(h, "bl", (uint8_t)s_pending_bl);
            (void)nvs_commit(h);
            nvs_close(h);
        }
    }
#endif
#else
    nvs_handle_t h;
    if (nvs_open("display", NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u8(h, "bl", s_backlight_percent);
    (void)nvs_commit(h);
    nvs_close(h);
#endif
}

static void load_touch_from_nvs() {
    nvs_handle_t h;
    if (nvs_open("touch", NVS_READONLY, &h) != ESP_OK) return;

    uint8_t mode = 0;
    uint32_t hz = 0;
    if (nvs_get_u8(h, "mode", &mode) == ESP_OK) s_touch_spi_mode = (mode <= 3) ? (int)mode : 0;
    if (nvs_get_u32(h, "hz", &hz) == ESP_OK && hz > 0) s_touch_spi_hz = (int)hz;

    uint8_t swap_xy = 0;
    if (nvs_get_u8(h, "swap", &swap_xy) == ESP_OK) s_touch_swap_xy = (swap_xy != 0);

    uint8_t mx = 0, my = 0;
    if (nvs_get_u8(h, "mx", &mx) == ESP_OK) s_touch_mirror_x = (mx != 0);
    if (nvs_get_u8(h, "my", &my) == ESP_OK) s_touch_mirror_y = (my != 0);

    uint8_t cal_en = 0;
    if (nvs_get_u8(h, "cal_en", &cal_en) == ESP_OK) s_touch_cal_enabled = (cal_en != 0);
    uint16_t v16 = 0;
    if (nvs_get_u16(h, "cal_l", &v16) == ESP_OK) s_touch_cal_left = v16;
    if (nvs_get_u16(h, "cal_r", &v16) == ESP_OK) s_touch_cal_right = v16;
    if (nvs_get_u16(h, "cal_t", &v16) == ESP_OK) s_touch_cal_top = v16;
    if (nvs_get_u16(h, "cal_b", &v16) == ESP_OK) s_touch_cal_bottom = v16;

    uint8_t aff_en = 0;
    if (nvs_get_u8(h, "aff_en", &aff_en) == ESP_OK) s_touch_affine_enabled = (aff_en != 0);
    float f = 0.0f;
    size_t sz = sizeof(float);
    if (nvs_get_blob(h, "aff_a", &f, &sz) == ESP_OK) s_touch_affine_a = f;
    sz = sizeof(float);
    if (nvs_get_blob(h, "aff_b", &f, &sz) == ESP_OK) s_touch_affine_b = f;
    sz = sizeof(float);
    if (nvs_get_blob(h, "aff_c", &f, &sz) == ESP_OK) s_touch_affine_c = f;
    sz = sizeof(float);
    if (nvs_get_blob(h, "aff_d", &f, &sz) == ESP_OK) s_touch_affine_d = f;
    sz = sizeof(float);
    if (nvs_get_blob(h, "aff_e", &f, &sz) == ESP_OK) s_touch_affine_e = f;
    sz = sizeof(float);
    if (nvs_get_blob(h, "aff_f", &f, &sz) == ESP_OK) s_touch_affine_f = f;

    uint8_t grid_en = 0;
    if (nvs_get_u8(h, "grid_en", &grid_en) == ESP_OK) s_touch_grid_enabled = (grid_en != 0);
    size_t grid_sz = sizeof(float) * s_touch_grid_dx.size();
    (void)nvs_get_blob(h, "grid_dx", s_touch_grid_dx.data(), &grid_sz);
    grid_sz = sizeof(float) * s_touch_grid_dy.size();
    (void)nvs_get_blob(h, "grid_dy", s_touch_grid_dy.data(), &grid_sz);

    int32_t pin = -1;
    if (nvs_get_i32(h, "sclk", &pin) == ESP_OK && GPIO_IS_VALID_GPIO((gpio_num_t)pin)) s_touch_sclk = (gpio_num_t)pin;
    pin = -1;
    if (nvs_get_i32(h, "mosi", &pin) == ESP_OK && GPIO_IS_VALID_GPIO((gpio_num_t)pin)) s_touch_mosi = (gpio_num_t)pin;
    pin = -1;
    if (nvs_get_i32(h, "miso", &pin) == ESP_OK && GPIO_IS_VALID_GPIO((gpio_num_t)pin)) s_touch_miso = (gpio_num_t)pin;
    nvs_close(h);
}

static void save_touch_to_nvs() {
#if CONFIG_IDF_TARGET_ESP32P4
    // On ESP32-P4, synchronous NVS writes (especially blobs) cause visible display flicker.
    // We defer touch settings save.
    xTaskCreate([](void*){
        nvs_handle_t h;
        if (nvs_open("touch", NVS_READWRITE, &h) == ESP_OK) {
            (void)nvs_set_u8(h, "mode", (uint8_t)s_touch_spi_mode);
            (void)nvs_set_u32(h, "hz", (uint32_t)s_touch_spi_hz);
            (void)nvs_set_u8(h, "swap", (uint8_t)(s_touch_swap_xy ? 1 : 0));
            (void)nvs_set_u8(h, "mx", (uint8_t)(s_touch_mirror_x ? 1 : 0));
            (void)nvs_set_u8(h, "my", (uint8_t)(s_touch_mirror_y ? 1 : 0));
            (void)nvs_set_u8(h, "cal_en", (uint8_t)(s_touch_cal_enabled ? 1 : 0));
            (void)nvs_set_u16(h, "cal_l", s_touch_cal_left);
            (void)nvs_set_u16(h, "cal_r", s_touch_cal_right);
            (void)nvs_set_u16(h, "cal_t", s_touch_cal_top);
            (void)nvs_set_u16(h, "cal_b", s_touch_cal_bottom);
            (void)nvs_set_u8(h, "aff_en", (uint8_t)(s_touch_affine_enabled ? 1 : 0));
            (void)nvs_set_blob(h, "aff_a", &s_touch_affine_a, sizeof(float));
            (void)nvs_set_blob(h, "aff_b", &s_touch_affine_b, sizeof(float));
            (void)nvs_set_blob(h, "aff_c", &s_touch_affine_c, sizeof(float));
            (void)nvs_set_blob(h, "aff_d", &s_touch_affine_d, sizeof(float));
            (void)nvs_set_blob(h, "aff_e", &s_touch_affine_e, sizeof(float));
            (void)nvs_set_blob(h, "aff_f", &s_touch_affine_f, sizeof(float));
            (void)nvs_set_u8(h, "grid_en", (uint8_t)(s_touch_grid_enabled ? 1 : 0));
            (void)nvs_set_blob(h, "grid_dx", s_touch_grid_dx.data(), sizeof(float) * s_touch_grid_dx.size());
            (void)nvs_set_blob(h, "grid_dy", s_touch_grid_dy.data(), sizeof(float) * s_touch_grid_dy.size());
            (void)nvs_set_i32(h, "sclk", (int32_t)s_touch_sclk);
            (void)nvs_set_i32(h, "mosi", (int32_t)s_touch_mosi);
            (void)nvs_set_i32(h, "miso", (int32_t)s_touch_miso);
            (void)nvs_commit(h);
            nvs_close(h);
        }
        vTaskDelete(NULL);
    }, "touch_save", 4096, nullptr, 2, NULL);
#else
    nvs_handle_t h;
    if (nvs_open("touch", NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u8(h, "mode", (uint8_t)s_touch_spi_mode);
    (void)nvs_set_u32(h, "hz", (uint32_t)s_touch_spi_hz);
    (void)nvs_set_u8(h, "swap", (uint8_t)(s_touch_swap_xy ? 1 : 0));
    (void)nvs_set_u8(h, "mx", (uint8_t)(s_touch_mirror_x ? 1 : 0));
    (void)nvs_set_u8(h, "my", (uint8_t)(s_touch_mirror_y ? 1 : 0));
    (void)nvs_set_u8(h, "cal_en", (uint8_t)(s_touch_cal_enabled ? 1 : 0));
    (void)nvs_set_u16(h, "cal_l", s_touch_cal_left);
    (void)nvs_set_u16(h, "cal_r", s_touch_cal_right);
    (void)nvs_set_u16(h, "cal_t", s_touch_cal_top);
    (void)nvs_set_u16(h, "cal_b", s_touch_cal_bottom);
    (void)nvs_set_u8(h, "aff_en", (uint8_t)(s_touch_affine_enabled ? 1 : 0));
    (void)nvs_set_blob(h, "aff_a", &s_touch_affine_a, sizeof(float));
    (void)nvs_set_blob(h, "aff_b", &s_touch_affine_b, sizeof(float));
    (void)nvs_set_blob(h, "aff_c", &s_touch_affine_c, sizeof(float));
    (void)nvs_set_blob(h, "aff_d", &s_touch_affine_d, sizeof(float));
    (void)nvs_set_blob(h, "aff_e", &s_touch_affine_e, sizeof(float));
    (void)nvs_set_blob(h, "aff_f", &s_touch_affine_f, sizeof(float));
    (void)nvs_set_u8(h, "grid_en", (uint8_t)(s_touch_grid_enabled ? 1 : 0));
    (void)nvs_set_blob(h, "grid_dx", s_touch_grid_dx.data(), sizeof(float) * s_touch_grid_dx.size());
    (void)nvs_set_blob(h, "grid_dy", s_touch_grid_dy.data(), sizeof(float) * s_touch_grid_dy.size());
    (void)nvs_set_i32(h, "sclk", (int32_t)s_touch_sclk);
    (void)nvs_set_i32(h, "mosi", (int32_t)s_touch_mosi);
    (void)nvs_set_i32(h, "miso", (int32_t)s_touch_miso);
    (void)nvs_commit(h);
    nvs_close(h);
#endif
}

static void apply_touch_grid(int &x, int &y) {
    if (!s_touch_grid_enabled) return;

    const float fx = (DISPLAY_WIDTH <= 1) ? 0.0f : (float)x / (float)(DISPLAY_WIDTH - 1);
    const float fy = (DISPLAY_HEIGHT <= 1) ? 0.0f : (float)y / (float)(DISPLAY_HEIGHT - 1);

    int gx = (int)std::floor(fx * 2.0f);
    int gy = (int)std::floor(fy * 2.0f);
    gx = std::clamp(gx, 0, 1);
    gy = std::clamp(gy, 0, 1);

    const float tx = std::clamp(fx * 2.0f - (float)gx, 0.0f, 1.0f);
    const float ty = std::clamp(fy * 2.0f - (float)gy, 0.0f, 1.0f);

    const auto idx = [](int ix, int iy) -> int { return iy * 3 + ix; };

    const int i00 = idx(gx + 0, gy + 0);
    const int i10 = idx(gx + 1, gy + 0);
    const int i01 = idx(gx + 0, gy + 1);
    const int i11 = idx(gx + 1, gy + 1);

    const float dx00 = s_touch_grid_dx[(size_t)i00];
    const float dx10 = s_touch_grid_dx[(size_t)i10];
    const float dx01 = s_touch_grid_dx[(size_t)i01];
    const float dx11 = s_touch_grid_dx[(size_t)i11];
    const float dy00 = s_touch_grid_dy[(size_t)i00];
    const float dy10 = s_touch_grid_dy[(size_t)i10];
    const float dy01 = s_touch_grid_dy[(size_t)i01];
    const float dy11 = s_touch_grid_dy[(size_t)i11];

    const auto lerp = [](float a, float b, float t) -> float { return a + (b - a) * t; };
    const float dx0 = lerp(dx00, dx10, tx);
    const float dx1 = lerp(dx01, dx11, tx);
    const float dy0 = lerp(dy00, dy10, tx);
    const float dy1 = lerp(dy01, dy11, tx);
    const float dx = lerp(dx0, dx1, ty);
    const float dy = lerp(dy0, dy1, ty);

    x = (int)std::lround((float)x + dx);
    y = (int)std::lround((float)y + dy);
    x = std::clamp(x, 0, DISPLAY_WIDTH - 1);
    y = std::clamp(y, 0, DISPLAY_HEIGHT - 1);
}

static void apply_touch_calibration(int &x, int &y) {
    if (!s_touch_cal_enabled) return;

    const int left = (int)s_touch_cal_left;
    const int right = (int)s_touch_cal_right;
    const int top = (int)s_touch_cal_top;
    const int bottom = (int)s_touch_cal_bottom;
    if (right - left < 20 || bottom - top < 20) return;

    x = (x - left) * (DISPLAY_WIDTH - 1) / (right - left);
    y = (y - top) * (DISPLAY_HEIGHT - 1) / (bottom - top);
    x = std::clamp(x, 0, DISPLAY_WIDTH - 1);
    y = std::clamp(y, 0, DISPLAY_HEIGHT - 1);

    if (s_touch_affine_enabled) {
        const float xf = (float)x;
        const float yf = (float)y;
        const float xa = s_touch_affine_a * xf + s_touch_affine_b * yf + s_touch_affine_c;
        const float ya = s_touch_affine_d * xf + s_touch_affine_e * yf + s_touch_affine_f;
        x = (int)std::lround(xa);
        y = (int)std::lround(ya);
        x = std::clamp(x, 0, DISPLAY_WIDTH - 1);
        y = std::clamp(y, 0, DISPLAY_HEIGHT - 1);
    }

    if (s_touch_grid_enabled) {
        apply_touch_grid(x, y);
    } else if (s_touch_quad_enabled) {
        const float yf = (float)y;
        const float yq = s_touch_quad_a * yf * yf + s_touch_quad_b * yf + s_touch_quad_c;
        y = (int)std::lround(yq);
        y = std::clamp(y, 0, DISPLAY_HEIGHT - 1);
    }
}

static void lcd_spi_pre_transfer_cb(spi_transaction_t *t) {
    const bool dc = t->user != nullptr;
    gpio_set_level(TFT_DC, dc ? 1 : 0);
}

static esp_err_t lcd_tx(const void *data, size_t len_bytes, bool dc) {
    if (!s_lcd_spi) return ESP_ERR_INVALID_STATE;

    spi_transaction_t t{};
    t.length = len_bytes * 8;
    t.tx_buffer = data;
    t.user = dc ? (void *)1 : nullptr;
    return spi_device_transmit(s_lcd_spi, &t);
}

static esp_err_t lcd_cmd(uint8_t cmd) { return lcd_tx(&cmd, 1, false); }

static esp_err_t lcd_data(const void *data, size_t len_bytes) {
    if (!data || len_bytes == 0) return ESP_OK;
    const uint8_t *p = static_cast<const uint8_t *>(data);
    size_t remaining = len_bytes;
    while (remaining > 0) {
        const size_t chunk = std::min(remaining, LCD_SPI_CHUNK_BYTES);
        const esp_err_t err = lcd_tx(p, chunk, true);
        if (err != ESP_OK) return err;
        p += chunk;
        remaining -= chunk;
    }
    return ESP_OK;
}

static esp_err_t lcd_write_u16be(uint16_t value) {
    uint8_t b[2] = {(uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return lcd_data(b, sizeof(b));
}

static esp_err_t lcd_set_addr_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    const uint16_t x1 = x + LCD_X_OFFSET;
    const uint16_t y1 = y + LCD_Y_OFFSET;
    const uint16_t x2 = x1 + w - 1;
    const uint16_t y2 = y1 + h - 1;

    ESP_RETURN_ON_ERROR(lcd_cmd(0x2A), TAG, "CASET");
    ESP_RETURN_ON_ERROR(lcd_write_u16be(x1), TAG, "CASET x1");
    ESP_RETURN_ON_ERROR(lcd_write_u16be(x2), TAG, "CASET x2");

    ESP_RETURN_ON_ERROR(lcd_cmd(0x2B), TAG, "PASET");
    ESP_RETURN_ON_ERROR(lcd_write_u16be(y1), TAG, "PASET y1");
    ESP_RETURN_ON_ERROR(lcd_write_u16be(y2), TAG, "PASET y2");

    return lcd_cmd(0x2C); // RAMWR
}

static esp_err_t lcd_set_rotation_landscape_1(void) {
    // ST7796 MADCTL bits:
    // MY 0x80, MX 0x40, MV 0x20, ML 0x10, BGR 0x08, MH 0x04.
    //
    // For 480x320 landscape on the S3 panel we use MY | MV | BGR.
    // The previous MX | MV variant mirrored both axes on this module.
    // Mirror flags are persisted in NVS and can be changed via API.
    // Most ST7796 modules are wired as BGR. If colors look "red=blue", keep BGR enabled.
    const uint8_t madctl_base = 0xA8; // MY | MV | BGR
    uint8_t madctl = madctl_base | (s_mirror_x ? 0x40 : 0) | (s_mirror_y ? 0x80 : 0);
    ESP_RETURN_ON_ERROR(lcd_cmd(0x36), TAG, "MADCTL cmd");
    return lcd_data(&madctl, 1);
}

static esp_err_t st7796_init_sequence(void) {
    ESP_LOGI(TAG, "Starting ST7796 initialization sequence...");
    
    // Init sequence adapted from LovyanGFX Panel_ST7796 defaults.
    const auto tx = [](uint8_t cmd, std::initializer_list<uint8_t> data, int delay_ms = 0) -> esp_err_t {
        ESP_LOGD(TAG, "ST7796 cmd 0x%02X, data size: %zu, delay: %d ms", cmd, data.size(), delay_ms);
        ESP_RETURN_ON_ERROR(lcd_cmd(cmd), TAG, "cmd 0x%02X", cmd);
        if (!data.size()) {
            if (delay_ms) vTaskDelay(pdMS_TO_TICKS(delay_ms));
            return ESP_OK;
        }
        std::array<uint8_t, 32> buf{};
        if (data.size() > buf.size()) return ESP_ERR_INVALID_SIZE;
        std::copy(data.begin(), data.end(), buf.begin());
        ESP_RETURN_ON_ERROR(lcd_data(buf.data(), data.size()), TAG, "data 0x%02X", cmd);
        if (delay_ms) vTaskDelay(pdMS_TO_TICKS(delay_ms));
        return ESP_OK;
    };

    ESP_RETURN_ON_ERROR(tx(0xF0, {0xC3}), TAG, "CSCON 1");
    ESP_RETURN_ON_ERROR(tx(0xF0, {0x96}), TAG, "CSCON 2");
    ESP_RETURN_ON_ERROR(tx(0xB4, {0x01}), TAG, "INVCTR");
    ESP_RETURN_ON_ERROR(tx(0xB6, {0x80, 0x22, 0x3B}), TAG, "DFUNCTR");
    ESP_RETURN_ON_ERROR(tx(0xE8, {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}), TAG, "DOCA");
    ESP_RETURN_ON_ERROR(tx(0xC1, {0x06}), TAG, "PWCTR2");
    ESP_RETURN_ON_ERROR(tx(0xC2, {0xA7}), TAG, "PWCTR3");
    ESP_RETURN_ON_ERROR(tx(0xC5, {0x18}, 120), TAG, "VMCTR");
    ESP_RETURN_ON_ERROR(tx(0xE0, {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B}), TAG, "GMCTRP1");
    ESP_RETURN_ON_ERROR(tx(0xE1, {0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B}, 120), TAG, "GMCTRN1");
    ESP_RETURN_ON_ERROR(tx(0xF0, {0x3C}), TAG, "CSCON off 1");
    ESP_RETURN_ON_ERROR(tx(0xF0, {0x69}), TAG, "CSCON off 2");

    // Exit sleep
    ESP_RETURN_ON_ERROR(tx(0x11, {}, 130), TAG, "SLPOUT");

    // Pixel format 16-bit (RGB565)
    ESP_RETURN_ON_ERROR(tx(0x3A, {0x55}), TAG, "COLMOD");

    // Rotation
    ESP_RETURN_ON_ERROR(lcd_set_rotation_landscape_1(), TAG, "rotation");

    // Idle off + display on
    ESP_RETURN_ON_ERROR(tx(0x38, {}), TAG, "IDMOFF");
    ESP_RETURN_ON_ERROR(tx(0x29, {}), TAG, "DISPON");
    
    ESP_LOGI(TAG, "ST7796 initialization sequence completed successfully");
    return ESP_OK;
}

static inline int backlight_gpio_level_from_on(bool on);

static void display_gpio_init(void) {
    gpio_config_t cfg{};
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pin_bit_mask = (1ULL << TFT_DC) | (1ULL << TFT_CS) | (1ULL << TFT_RST) | (1ULL << TFT_BL) | (1ULL << TOUCH_CS);
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&cfg);

    gpio_set_level(TFT_CS, 1);
    gpio_set_level(TOUCH_CS, 1);
    gpio_set_level(TFT_DC, 0);
    gpio_set_level(TFT_BL, backlight_gpio_level_from_on(false));

    if (TOUCH_IRQ != GPIO_NUM_NC && GPIO_IS_VALID_GPIO(TOUCH_IRQ)) {
        gpio_config_t icfg{};
        icfg.mode = GPIO_MODE_INPUT;
        icfg.pin_bit_mask = (1ULL << TOUCH_IRQ);
        icfg.pull_up_en = GPIO_PULLUP_ENABLE;   // XPT2046 IRQ is active-low (needs pull-up)
        icfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_config(&icfg);
    }
}

static uint32_t backlight_duty_from_percent(uint8_t percent) {
    if (percent >= 100) return (1u << kBacklightDutyRes) - 1u;
    return (uint32_t)percent * (((1u << kBacklightDutyRes) - 1u)) / 100u;
}

static inline int backlight_gpio_level_from_on(bool on) {
    return on ? (TFT_BL_ACTIVE_LOW ? 0 : 1) : (TFT_BL_ACTIVE_LOW ? 1 : 0);
}

static void backlight_apply() {
    if (TFT_BL == GPIO_NUM_NC || !GPIO_IS_VALID_OUTPUT_GPIO(TFT_BL)) return;
    if (!s_backlight_pwm_enabled || !s_backlight_inited) {
        gpio_set_level(TFT_BL, backlight_gpio_level_from_on(s_backlight_percent > 0));
        return;
    }
    const uint32_t duty_raw = backlight_duty_from_percent(s_backlight_percent);
    const uint32_t max_duty = (1u << (uint32_t)kBacklightDutyRes) - 1u;
    const uint32_t duty = TFT_BL_ACTIVE_LOW ? (max_duty - duty_raw) : duty_raw;
    (void)ledc_set_duty(kBacklightMode, kBacklightChannel, duty);
    (void)ledc_update_duty(kBacklightMode, kBacklightChannel);
}

static void backlight_init() {
    if (TFT_BL == GPIO_NUM_NC || !GPIO_IS_VALID_OUTPUT_GPIO(TFT_BL)) {
        ESP_LOGW(TAG, "Backlight pin not available");
        return;
    }
    if (!s_backlight_pwm_enabled) {
        gpio_set_level(TFT_BL, backlight_gpio_level_from_on(s_backlight_percent > 0));
        return;
    }

    ledc_timer_config_t timer = {};
    timer.speed_mode = kBacklightMode;
    timer.timer_num = kBacklightTimer;
    timer.duty_resolution = kBacklightDutyRes;
    timer.freq_hz = kBacklightPwmHz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK) {
        ESP_LOGW(TAG, "Backlight ledc_timer_config failed");
        return;
    }

    ledc_channel_config_t ch = {};
    ch.speed_mode = kBacklightMode;
    ch.channel = kBacklightChannel;
    ch.timer_sel = kBacklightTimer;
    ch.gpio_num = (int)TFT_BL;
    ch.duty = 0;
    ch.hpoint = 0;
    if (ledc_channel_config(&ch) != ESP_OK) {
        ESP_LOGW(TAG, "Backlight ledc_channel_config failed");
        return;
    }

    s_backlight_inited = true;
    backlight_apply();
}

static esp_err_t display_spi_init(void) {
    ESP_LOGI(TAG, "Initializing SPI bus for display...");
    ESP_LOGI(TAG, "SPI pins: SCLK=%d, MOSI=%d, MISO=%d, CS=%d, DC=%d, RST=%d, BL=%d", 
             TFT_SCLK, TFT_MOSI, TFT_MISO, TFT_CS, TFT_DC, TFT_RST, TFT_BL);
    
    spi_bus_config_t buscfg{};
    buscfg.sclk_io_num = TFT_SCLK;
    buscfg.mosi_io_num = TFT_MOSI;
    buscfg.miso_io_num = TFT_MISO;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = (int)LCD_SPI_CHUNK_BYTES + 8;

    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi_bus_initialize");

    spi_device_interface_config_t devcfg{};
    devcfg.clock_speed_hz = TFT_SPI_CLOCK_HZ;
    devcfg.mode = 0;
    devcfg.spics_io_num = TFT_CS;
    devcfg.queue_size = TFT_SPI_QUEUE_SIZE;
    devcfg.pre_cb = lcd_spi_pre_transfer_cb;
    devcfg.flags = SPI_DEVICE_HALFDUPLEX;

    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_HOST, &devcfg, &s_lcd_spi), TAG, "spi_bus_add_device lcd");
    
    ESP_LOGI(TAG, "SPI display device initialized successfully");

    const bool touch_shares_bus = (s_touch_sclk == TFT_SCLK) && (s_touch_mosi == TFT_MOSI) && (s_touch_miso == TFT_MISO);
    s_touch_host = touch_shares_bus ? LCD_HOST : TOUCH_HOST;

    if (!touch_shares_bus) {
        ESP_LOGI(TAG, "Touch controller uses separate SPI bus");
        spi_bus_config_t tbus{};
        tbus.sclk_io_num = s_touch_sclk;
        tbus.mosi_io_num = s_touch_mosi;
        tbus.miso_io_num = s_touch_miso;
        tbus.quadwp_io_num = -1;
        tbus.quadhd_io_num = -1;
        tbus.max_transfer_sz = 64;
        ESP_RETURN_ON_ERROR(spi_bus_initialize(TOUCH_HOST, &tbus, SPI_DMA_CH_AUTO), TAG, "spi_bus_initialize touch");
    }

    return ESP_OK;
}

static void display_reset(void) {
    ESP_LOGI(TAG, "Resetting display controller...");
    gpio_set_level(TFT_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "Display reset completed");
}

static esp_err_t touch_deinit(void) {
    if (s_touch) {
        (void)esp_lcd_touch_del(s_touch);
        s_touch = nullptr;
    }
    if (s_touch_io) {
        (void)esp_lcd_panel_io_del(s_touch_io);
        s_touch_io = nullptr;
    }
#if CONFIG_IDF_TARGET_ESP32P4
    if (s_p4_touch_i2c_bus) {
        (void)i2c_del_master_bus(s_p4_touch_i2c_bus);
        s_p4_touch_i2c_bus = nullptr;
    }
#endif
    return ESP_OK;
}

static esp_err_t touch_init(void) {
    if (s_touch) return ESP_OK;
    if (!s_touch_mutex) {
        s_touch_mutex = xSemaphoreCreateMutex();
    }

#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = I2C_NUM_0;
        i2c_bus_cfg.sda_io_num = P4_TOUCH_SDA_GPIO;
        i2c_bus_cfg.scl_io_num = P4_TOUCH_SCL_GPIO;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_bus_cfg, &s_p4_touch_i2c_bus), TAG, "p4 touch i2c");

        // #region debug-point touch-not-working/i2c-scan
        p4_i2c_scan(s_p4_touch_i2c_bus);
        // #endregion debug-point touch-not-working/i2c-scan

        esp_err_t last_err = ESP_FAIL;
        for (uint8_t addr : P4_GT911_ADDR_CANDIDATES) {
            esp_lcd_panel_io_i2c_config_t tp_io_cfg = {};
            tp_io_cfg.scl_speed_hz = 100000;
            tp_io_cfg.dev_addr = addr;
            tp_io_cfg.control_phase_bytes = 1;
            tp_io_cfg.dc_bit_offset = 0;
            tp_io_cfg.lcd_cmd_bits = 16;
            tp_io_cfg.lcd_param_bits = 0;
            tp_io_cfg.flags.disable_control_phase = 1;
            const esp_err_t io_err = esp_lcd_new_panel_io_i2c(s_p4_touch_i2c_bus, &tp_io_cfg, &s_touch_io);
            if (io_err != ESP_OK) {
                last_err = io_err;
                continue;
            }

            esp_lcd_touch_config_t tp_cfg = {};
            tp_cfg.x_max = P4_PANEL_PHYS_W;
            tp_cfg.y_max = P4_PANEL_PHYS_H;
            tp_cfg.rst_gpio_num = GPIO_NUM_NC;
            tp_cfg.int_gpio_num = GPIO_NUM_NC;
            tp_cfg.levels.reset = 0;
            tp_cfg.levels.interrupt = 0;
            tp_cfg.flags.swap_xy = 0;
            tp_cfg.flags.mirror_x = 0;
            tp_cfg.flags.mirror_y = 0;
            tp_cfg.process_coordinates = nullptr;
            tp_cfg.interrupt_callback = nullptr;
            tp_cfg.user_data = nullptr;
            tp_cfg.driver_data = nullptr;

            const esp_err_t err = esp_lcd_touch_new_i2c_gt911(s_touch_io, &tp_cfg, &s_touch);
            if (err == ESP_OK && s_touch != nullptr) {
                ESP_LOGI(TAG, "Touch initialized (GT911, I2C addr=0x%02X, SDA=%d SCL=%d)", (unsigned)addr, (int)P4_TOUCH_SDA_GPIO, (int)P4_TOUCH_SCL_GPIO);
                last_err = ESP_OK;
                break;
            }
            last_err = err;
            if (s_touch_io) {
                (void)esp_lcd_panel_io_del(s_touch_io);
                s_touch_io = nullptr;
            }
        }
        if (last_err != ESP_OK || s_touch == nullptr) {
            (void)touch_deinit();
            return last_err;
        }

        s_touch_swap_xy = false;
        s_touch_mirror_x = false;
        s_touch_mirror_y = false;
        return ESP_OK;
    }
#endif

    esp_lcd_panel_io_spi_config_t io_cfg{};
    io_cfg.cs_gpio_num = (gpio_num_t)TOUCH_CS;
    io_cfg.dc_gpio_num = (gpio_num_t)GPIO_NUM_NC;
    io_cfg.spi_mode = s_touch_spi_mode;
    io_cfg.pclk_hz = (unsigned int)s_touch_spi_hz;
    io_cfg.trans_queue_depth = 3;
    io_cfg.on_color_trans_done = nullptr;
    io_cfg.user_ctx = nullptr;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.cs_ena_pretrans = 0;
    io_cfg.cs_ena_posttrans = 0;
    io_cfg.flags.dc_high_on_cmd = 0;
    io_cfg.flags.dc_low_on_data = 0;
    io_cfg.flags.dc_low_on_param = 0;
    io_cfg.flags.octal_mode = 0;
    io_cfg.flags.quad_mode = 0;
    io_cfg.flags.sio_mode = 0;
    io_cfg.flags.lsb_first = 0;
    io_cfg.flags.cs_high_active = 0;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)s_touch_host, &io_cfg, &s_touch_io), TAG, "touch io");

    // Base mapping (panel wiring): swap axes + invert Y.
    // Then apply display mirror settings so touch stays aligned with what the user sees.
    // Our display "mirror_y" flag effectively toggles the baseline Y inversion.
    esp_lcd_touch_config_t tp_cfg{};
    tp_cfg.x_max = DISPLAY_WIDTH - 1;
    tp_cfg.y_max = DISPLAY_HEIGHT - 1;
    tp_cfg.rst_gpio_num = GPIO_NUM_NC;
    tp_cfg.int_gpio_num = GPIO_NUM_NC;
    tp_cfg.levels.reset = 0;
    tp_cfg.levels.interrupt = 0;
    tp_cfg.flags.swap_xy = s_touch_swap_xy ? 1 : 0;
    tp_cfg.flags.mirror_x = s_touch_mirror_x ? 1 : 0;
    tp_cfg.flags.mirror_y = s_touch_mirror_y ? 1 : 0;
    tp_cfg.process_coordinates = nullptr;
    tp_cfg.interrupt_callback = nullptr;
    tp_cfg.user_data = nullptr;
    tp_cfg.driver_data = nullptr;

    const esp_err_t err = esp_lcd_touch_new_spi_xpt2046(s_touch_io, &tp_cfg, &s_touch);
    if (err != ESP_OK) {
        (void)touch_deinit();
        return err;
    }

    ESP_LOGI(TAG, "Touch initialized (XPT2046, mode=%d, hz=%d, host=%d)", s_touch_spi_mode, s_touch_spi_hz, (int)s_touch_host);
    return ESP_OK;
}

static bool touch_read_once(int &x, int &y, uint16_t &z) {
    if (!s_touch) return false;
#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        // #region debug-point touch-not-working/gt911-read
        const esp_err_t err = esp_lcd_touch_read_data(s_touch);
        if (err != ESP_OK) {
            static bool s_p4_touch_read_err_logged = false;
            if (!s_p4_touch_read_err_logged) {
                s_p4_touch_read_err_logged = true;
                ESP_LOGE(TAG, "P4 touch read_data failed: %s", esp_err_to_name(err));
            }
            x = 0;
            y = 0;
            z = 0;
            return false;
        }
        esp_lcd_touch_point_data_t points[1]{};
        uint8_t point_cnt = 0;
        (void)esp_lcd_touch_get_data(s_touch, points, &point_cnt, 1);
        static bool s_prev_pressed = false;
        const bool pressed_dbg = (point_cnt > 0);
        if (pressed_dbg != s_prev_pressed) {
            s_prev_pressed = pressed_dbg;
            if (point_cnt > 0) {
                ESP_LOGI(TAG, "DBG_TOUCH gt911 pressed=1 x=%u y=%u z=%u",
                         (unsigned)points[0].x,
                         (unsigned)points[0].y,
                         (unsigned)points[0].strength);
            } else {
                ESP_LOGI(TAG, "DBG_TOUCH gt911 pressed=0");
            }
        }
        if (point_cnt == 0) {
            (void)esp_lcd_touch_read_data(s_touch);
            (void)esp_lcd_touch_get_data(s_touch, points, &point_cnt, 1);
            x = 0;
            y = 0;
            z = 0;
            if (point_cnt == 0) return false;
        }
        x = (int)points[0].x;
        y = (int)points[0].y;
        z = points[0].strength;
        return true;
        // #endregion debug-point touch-not-working/gt911-read
    }
#endif
    bool locked = false;
    if (s_touch_mutex) {
        locked = (xSemaphoreTake(s_touch_mutex, pdMS_TO_TICKS(20)) == pdTRUE);
    }

    (void)esp_lcd_touch_read_data(s_touch);
    esp_lcd_touch_point_data_t points[1]{};
    uint8_t point_cnt = 0;
    (void)esp_lcd_touch_get_data(s_touch, points, &point_cnt, 1);
    static bool s_prev_pressed_dbg = false;
    static uint64_t s_last_dbg_ms = 0;
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    const bool pressed_dbg = (point_cnt > 0);

    if (!pressed_dbg) {
        x = 0;
        y = 0;
        z = 0;
        if (locked) xSemaphoreGive(s_touch_mutex);
        if (pressed_dbg != s_prev_pressed_dbg || (now_ms - s_last_dbg_ms) >= 700) {
            s_prev_pressed_dbg = pressed_dbg;
            s_last_dbg_ms = now_ms;
            ESP_LOGI(TAG, "DBG_TOUCH xpt2046 pressed=0");
        }
        return false;
    }

    x = (int)points[0].x;
    y = (int)points[0].y;
    z = points[0].strength;
    if (locked) xSemaphoreGive(s_touch_mutex);

    if (pressed_dbg != s_prev_pressed_dbg || (now_ms - s_last_dbg_ms) >= 700) {
        s_prev_pressed_dbg = pressed_dbg;
        s_last_dbg_ms = now_ms;
        ESP_LOGI(TAG, "DBG_TOUCH xpt2046 pressed=1 x=%d y=%d z=%u", x, y, (unsigned)z);
    }
    return true;
}

static int median3(int a, int b, int c) {
    if (a > b) std::swap(a, b);
    if (b > c) std::swap(b, c);
    if (a > b) std::swap(a, b);
    return b;
}

void display_driver_init(void) {
#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        ESP_LOGI(TAG, "BOOT_FLOW step=driver_init_begin board=new_p4");
        load_mirror_from_nvs();
        ESP_LOGI(TAG, "BOOT_FLOW step=nvs_display_loaded brightness=%u", (unsigned)s_backlight_percent);
        p4_backlight_init();
        // Keep backlight OFF while the panel is being brought up (matches reference init and avoids power oscillations).
        p4_backlight_apply_percent(0);
        ESP_LOGI(TAG, "BOOT_FLOW step=backlight_forced_off");
        s_p4_backlight_deferred_until_first_frame = true;
        const esp_err_t p4_err = p4_display_init();
        if (p4_err != ESP_OK) {
            ESP_LOGE(TAG, "P4 display init failed: %s", esp_err_to_name(p4_err));
            return;
        }
        ESP_LOGI(TAG, "BOOT_FLOW step=panel_init_done");
        const esp_err_t tp_err = touch_init();
        if (tp_err != ESP_OK) {
            ESP_LOGW(TAG, "P4 touch init failed: %s", esp_err_to_name(tp_err));
        }
        ESP_LOGI(TAG, "BOOT_FLOW step=touch_init_done");
        // Do not enable backlight here. Slint will call display_driver_show_backlight_after_first_frame()
        // after persistent data is loaded, AppState is built, and the first complete frame is submitted.
        ESP_LOGI(TAG, "BOOT_FLOW step=driver_init_end backlight_deferred=1");
        ESP_LOGI(TAG, "Display initialized successfully (P4)");
        return;
    }
#endif

    load_mirror_from_nvs();
#if !CONFIG_IDF_TARGET_ESP32P4
    normalize_s3_default_orientation();
#endif

    s_touch_swap_xy = true;
    s_touch_mirror_x = s_mirror_x;
    s_touch_mirror_y = s_mirror_y ? false : true;

    load_touch_from_nvs();
    display_gpio_init();
    backlight_init();
    
    esp_err_t err = display_spi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(err));
        return;
    }
    
    display_reset();
    
    err = st7796_init_sequence();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ST7796 controller: %s", esp_err_to_name(err));
        gpio_set_level(TFT_BL, backlight_gpio_level_from_on(false));
        return;
    }
    
    err = touch_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize touch controller: %s", esp_err_to_name(err));
    }

    display_driver_set_backlight_percent(s_backlight_percent);
    ESP_LOGI(TAG, "Display initialized successfully");
}

uint8_t display_driver_get_backlight_percent(void) {
    return s_backlight_percent;
}

static void display_driver_set_backlight_percent_internal(uint8_t percent, bool from_ui, bool forced) {
#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        const uint8_t clamped = (uint8_t)std::clamp<int>(percent, 0, 100);
        // After first-frame handoff, NewP4 backlight must only be changed by explicit UI brightness action.
        // This blocks side-effect BL pulses from runtime/system events (wifi/storage/render transitions).
        if (s_p4_backlight_runtime_locked && !from_ui) {
            ESP_LOGI(TAG, "BACKLIGHT skip locked req=%u forced=%d current=%u",
                     (unsigned)clamped,
                     (int)forced,
                     (unsigned)s_backlight_percent);
            return;
        }
        if (s_p4_backlight_deferred_until_first_frame && clamped > 0 && !forced) {
            s_backlight_percent = clamped;
            ESP_LOGI(TAG, "BACKLIGHT defer until first frame req=%u", (unsigned)clamped);
            return;
        }
        if (clamped == s_backlight_percent) {
            return;
        }
        static uint64_t last_log_ms = 0;
        static int last_req = -1;
        const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        if ((int)clamped != last_req || (now_ms - last_log_ms) > 1000) {
            last_log_ms = now_ms;
            last_req = (int)clamped;
            ESP_LOGI(TAG, "P4 BL req=%d (forced=%d)", (int)clamped, (int)forced);
        }
        s_backlight_percent = clamped;
        p4_backlight_apply_percent(s_backlight_percent);
        if (from_ui) {
            save_backlight_to_nvs();
        }
        return;
    }
#endif
    if (percent < 50 && percent > 0) percent = 50;
    if (percent > 100) percent = 100;
    if (percent == s_backlight_percent) return;
    s_backlight_percent = percent;
    backlight_apply();
    if (from_ui) {
        save_backlight_to_nvs();
    }
}

void display_driver_set_backlight_percent(uint8_t percent) {
    // System/runtime path: never persist and never override runtime lock on NewP4.
    display_driver_set_backlight_percent_internal(percent, false, false);
}

void display_driver_set_backlight_percent_from_ui(uint8_t percent) {
    // UI path: explicit user-requested brightness change.
    display_driver_set_backlight_percent_internal(percent, true, false);
}

void display_driver_set_backlight_percent_forced(uint8_t percent) {
    // Forced path: bypass lock (used for transitions) but don't persist.
    display_driver_set_backlight_percent_internal(percent, false, true);
}

void display_driver_show_backlight_after_first_frame(uint8_t percent) {
#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        const uint8_t clamped = (uint8_t)std::clamp<int>(percent, 0, 100);
        static bool s_first_frame_bl_applied = false;
        if (s_first_frame_bl_applied) {
            ESP_LOGI(TAG, "BACKLIGHT first_frame_ready skip already_applied=%u", (unsigned)clamped);
            return;
        }
        s_p4_backlight_deferred_until_first_frame = false;
        // Lock runtime/system backlight updates after first frame.
        // Only explicit UI brightness changes are allowed to modify BL.
        s_p4_backlight_runtime_locked = true;
        s_backlight_percent = clamped;
        s_first_frame_bl_applied = true;
        ESP_LOGI(TAG, "BACKLIGHT first_frame_ready percent=%u", (unsigned)clamped);
        p4_backlight_apply_percent(s_backlight_percent);
        return;
    }
#endif
    display_driver_set_backlight_percent_internal(percent, false, true);
}

void display_driver_persist_backlight_now(void) {
#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        nvs_handle_t h;
        if (nvs_open("display", NVS_READWRITE, &h) != ESP_OK) {
            ESP_LOGW(TAG, "BACKLIGHT persist failed: nvs_open");
            return;
        }
        const esp_err_t s_ok = nvs_set_u8(h, "bl", s_backlight_percent);
        if (s_ok == ESP_OK) (void)nvs_commit(h);
        nvs_close(h);
        return;
    }
#endif
    save_backlight_to_nvs();
}

bool display_driver_blit_rgb565(int x, int y, int w, int h, const uint16_t *data) {
#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        if (!s_p4_display_ready || s_p4_panel == nullptr || !data || w <= 0 || h <= 0) return false;
        if (kP4RenderTrace) {
            const uint64_t ts_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
            ESP_LOGI(TAG, "RENDER blit_rgb565 ts=%llu ms frame=%dx%d pos=(%d,%d)",
                     (unsigned long long)ts_ms, w, h, x, y);
        }

        const int logical_w = P4_RENDER_W;
        const int logical_h = P4_RENDER_H;
        int x1 = x;
        int y1 = y;
        int x2 = x + w - 1;
        int y2 = y + h - 1;
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 >= logical_w) x2 = logical_w - 1;
        if (y2 >= logical_h) y2 = logical_h - 1;
        if (x2 < x1 || y2 < y1) return false;

        const int copy_w = x2 - x1 + 1;
        const int copy_h = y2 - y1 + 1;

        // Rotate landscape (800x480) into portrait panel (480x800).
        const int px1 = y1;
        const int px2 = y2;
        const int py1 = (P4_PANEL_PHYS_H - 1) - x2;
        const int py2 = (P4_PANEL_PHYS_H - 1) - x1;
        if (px1 < 0 || py1 < 0 || px2 >= P4_PANEL_PHYS_W || py2 >= P4_PANEL_PHYS_H) return false;

        const int rot_w = copy_h;
        const int rot_h = copy_w;
        const size_t rot_need = (size_t)rot_w * (size_t)rot_h;
        if (rot_need == 0 || rot_need > s_p4_submit_buf_capacity_px) return false;

        // Never submit directly from UI thread.
        // Queue latest region for single display worker (last-write-wins).
        if (!s_p4_submit_mutex || !s_p4_submit_task) {
            return false;
        }
        uint16_t *dst_buf = nullptr;
        for (int tries = 0; tries < 50 && !dst_buf; ++tries) {
            if (xSemaphoreTake(s_p4_submit_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
                return false;
            }
            for (auto *b : s_p4_submit_bufs) {
                if (b && b != s_p4_submit_inflight_buf && b != s_p4_submit_pending_buf) {
                    dst_buf = b;
                    break;
                }
            }
            xSemaphoreGive(s_p4_submit_mutex);
            if (!dst_buf) {
                vTaskDelay(1);
            }
        }
        if (!dst_buf) return false;

        for (int row = 0; row < copy_h; ++row) {
            const int ly = y1 + row;
            const int src_row = (ly - y);
            const uint16_t *src = data + ((size_t)src_row * (size_t)w) + (size_t)(x1 - x);
            for (int col = 0; col < copy_w; ++col) {
                const int lx = x1 + col;
                const int px = ly;
                const int py = (P4_PANEL_PHYS_H - 1) - lx;
                const int local_x = px - px1;
                const int local_y = py - py1;
                dst_buf[(size_t)local_y * (size_t)rot_w + (size_t)local_x] = src[col];
            }
        }

        if (xSemaphoreTake(s_p4_submit_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
            return false;
        }
        s_p4_submit_x1 = px1;
        s_p4_submit_y1 = py1;
        s_p4_submit_x2 = px2;
        s_p4_submit_y2 = py2;
        s_p4_submit_pending_buf = dst_buf;
        s_p4_submit_pending_px = rot_need;
        s_p4_submit_pending = true;
        xSemaphoreGive(s_p4_submit_mutex);
        xTaskNotifyGive(s_p4_submit_task);
        return true;
    }
#endif

    if (!data || w <= 0 || h <= 0) return false;
    LcdFlushGuard flush_guard;

    int x1 = x + s_lcd_x_offset;
    int y1 = y + s_lcd_y_offset;
    int x2 = x1 + w - 1;
    int y2 = y1 + h - 1;

    int crop_left = 0;
    int crop_top = 0;
    int crop_right = 0;
    int crop_bottom = 0;

    if (x1 < 0) {
        crop_left = -x1;
        x1 = 0;
    }
    if (y1 < 0) {
        crop_top = -y1;
        y1 = 0;
    }
    if (x2 >= DISPLAY_WIDTH) crop_right = x2 - (DISPLAY_WIDTH - 1);
    if (y2 >= DISPLAY_HEIGHT) crop_bottom = y2 - (DISPLAY_HEIGHT - 1);

    const int out_w = w - crop_left - crop_right;
    const int out_h = h - crop_top - crop_bottom;
    if (out_w <= 0 || out_h <= 0) return false;

    if (lcd_set_addr_window((uint16_t)x1, (uint16_t)y1, (uint16_t)out_w, (uint16_t)out_h) != ESP_OK) {
        return false;
    }

    if (!s_lcd_swap_bytes) {
        if (crop_left == 0 && crop_right == 0) {
            const uint8_t *buf = (const uint8_t *)(data + (crop_top * w));
            const size_t bytes = (size_t)w * (size_t)out_h * 2;
            (void)lcd_data(buf, bytes);
        } else {
            for (int row = 0; row < out_h; ++row) {
                const int src_row = crop_top + row;
                const uint8_t *buf = (const uint8_t *)(data + (src_row * w + crop_left));
                (void)lcd_data(buf, (size_t)out_w * 2);
            }
        }
        return true;
    }

    // Byte-swap RGB565 pixels into a scratch row buffer before writing.
    static std::vector<uint16_t> rowbuf;
    if ((int)rowbuf.size() < out_w) rowbuf.resize((size_t)out_w);

    for (int row = 0; row < out_h; ++row) {
        const int src_row = crop_top + row;
        const uint16_t *src = data + (src_row * w + crop_left);
        for (int col = 0; col < out_w; ++col) {
            const uint16_t px = src[col];
            rowbuf[(size_t)col] = (uint16_t)((px >> 8) | (px << 8));
        }
        (void)lcd_data((const uint8_t *)rowbuf.data(), (size_t)out_w * 2);
    }

    return true;
}

bool display_driver_p4_draw_bitmap_native(int x1, int y1, int x2_exclusive, int y2_exclusive, const uint16_t *data) {
#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        if (!s_p4_display_ready || s_p4_panel == nullptr || !data) return false;
        if (x1 < 0 || y1 < 0 || x2_exclusive <= x1 || y2_exclusive <= y1) return false;
        if (x2_exclusive > P4_PANEL_PHYS_W || y2_exclusive > P4_PANEL_PHYS_H) return false;
        const esp_err_t err = esp_lcd_panel_draw_bitmap(s_p4_panel, x1, y1, x2_exclusive, y2_exclusive, data);
        return err == ESP_OK;
    }
#endif
    (void)x1; (void)y1; (void)x2_exclusive; (void)y2_exclusive; (void)data;
    return false;
}

void display_driver_set_blit_fence_enabled(bool enabled) {
#if CONFIG_IDF_TARGET_ESP32P4
    s_p4_blit_fence_enabled = enabled;
    ESP_LOGI(TAG, "RENDER_PROFILE: blit_fence_enabled=%d", enabled ? 1 : 0);
#else
    (void)enabled;
#endif
}

bool display_driver_touch_probe(uint16_t *raw_x, uint16_t *raw_y, uint16_t *z1) {
    if (!s_touch) {
        if (raw_x) *raw_x = 0;
        if (raw_y) *raw_y = 0;
        if (z1) *z1 = 0;
        return false;
    }

#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        bool locked = false;
        if (s_touch_mutex) {
            locked = (xSemaphoreTake(s_touch_mutex, pdMS_TO_TICKS(10)) == pdTRUE);
        }
        const int logical_w = P4_UI_LOGICAL_W;
        const int logical_h = P4_UI_LOGICAL_H;
        int px = 0;
        int py = 0;
        uint16_t pz = 0;
        const bool raw_pressed = touch_read_once(px, py, pz);
        bool pressed = raw_pressed;
        int lx = 0;
        int ly = 0;
        if (pressed) {
            px = std::clamp(px, 0, P4_PANEL_PHYS_W - 1);
            py = std::clamp(py, 0, P4_PANEL_PHYS_H - 1);
            int lx_landscape = (P4_PANEL_PHYS_H - 1) - py; // 0..799
            int ly_landscape = px;                         // 0..479
            lx_landscape = std::clamp(lx_landscape, 0, P4_LANDSCAPE_W - 1);
            ly_landscape = std::clamp(ly_landscape, 0, P4_LANDSCAPE_H - 1);
            lx = (lx_landscape * P4_UI_LOGICAL_W) / P4_LANDSCAPE_W;
            ly = (ly_landscape * P4_UI_LOGICAL_H) / P4_LANDSCAPE_H;
            lx = std::clamp(lx, 0, logical_w - 1);
            ly = std::clamp(ly, 0, logical_h - 1);

        }

        if (pressed) {
            s_touch_press_streak = 1;
            s_touch_release_streak = 0;
            s_touch_debounced = true;
        } else {
            s_touch_release_streak = 1;
            s_touch_press_streak = 0;
            s_touch_debounced = false;
            lx = 0;
            ly = 0;
            pz = 0;
        }
        s_touch_last_pressed = s_touch_debounced;
        s_touch_last_raw_x = s_touch_debounced ? (uint16_t)lx : 0;
        s_touch_last_raw_y = s_touch_debounced ? (uint16_t)ly : 0;
        s_touch_last_z1 = s_touch_debounced ? pz : 0;
        static uint64_t s_last_tp_log_ms = 0;
        const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        if (now_ms - s_last_tp_log_ms >= 800) {
            s_last_tp_log_ms = now_ms;
            ESP_LOGI(TAG, "P4 TP pressed=%d px=%d py=%d lx=%d ly=%d z=%u", (int)s_touch_debounced, px, py, lx, ly, (unsigned)pz);
        }
        if (raw_x) *raw_x = s_touch_last_raw_x;
        if (raw_y) *raw_y = s_touch_last_raw_y;
        if (z1) *z1 = s_touch_last_z1;
        if (locked) {
            xSemaphoreGive(s_touch_mutex);
        }
        return s_touch_debounced;
    }
#endif

    const bool pressed = true;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    uint16_t z0 = 0, z1v = 0, z2 = 0;
    if (!touch_read_once(x0, y0, z0)) {
        if (raw_x) *raw_x = 0;
        if (raw_y) *raw_y = 0;
        if (z1) *z1 = 0;
        s_touch_last_pressed = false;
        return false;
    }
    // Take a couple of extra samples to reduce noise.
    if (!touch_read_once(x1, y1, z1v)) {
        x1 = x0;
        y1 = y0;
        z1v = z0;
    }
    if (!touch_read_once(x2, y2, z2)) {
        x2 = x0;
        y2 = y0;
        z2 = z0;
    }

    int lx = median3(x0, x1, x2);
    int ly = median3(y0, y1, y2);
    uint16_t lz = (uint16_t)median3((int)z0, (int)z1v, (int)z2);
    const int raw_x_min = std::min({x0, x1, x2});
    const int raw_x_max = std::max({x0, x1, x2});
    const int raw_y_min = std::min({y0, y1, y2});
    const int raw_y_max = std::max({y0, y1, y2});
    const bool sample_noisy =
        (raw_x_max - raw_x_min) > kTouchSampleSpanMax || (raw_y_max - raw_y_min) > kTouchSampleSpanMax;

    // Depending on XPT2046 config, driver may return raw ADC (0..4095) or already-scaled coords.
    // Clamp broadly, then apply our calibration into display coordinates.
    static constexpr int RAW_MAX = 4095;
    lx = std::clamp(lx, 0, RAW_MAX);
    ly = std::clamp(ly, 0, RAW_MAX);
    apply_touch_calibration(lx, ly);

    const bool filtered_pressed =
        pressed && (lz >= kTouchMinZ1) && !(sample_noisy && lz < kTouchStableZ1);
    if (filtered_pressed && s_touch_last_pressed) {
        const int dx = lx - (int)s_touch_last_raw_x;
        const int dy = ly - (int)s_touch_last_raw_y;
        if ((std::abs(dx) > kTouchSpikeMax || std::abs(dy) > kTouchSpikeMax) && lz < kTouchSpikeWeakZ1) {
            lx = (int)s_touch_last_raw_x;
            ly = (int)s_touch_last_raw_y;
            lz = s_touch_last_z1;
        }
    }
    if (filtered_pressed) {
        s_touch_press_streak = std::min<uint8_t>(s_touch_press_streak + 1, 255);
        s_touch_release_streak = 0;
        if (s_touch_press_streak >= kTouchDebounceSamples) s_touch_debounced = true;
    } else {
        s_touch_release_streak = std::min<uint8_t>(s_touch_release_streak + 1, 255);
        s_touch_press_streak = 0;
        if (s_touch_release_streak >= kTouchDebounceSamples) s_touch_debounced = false;
    }

    s_touch_last_pressed = s_touch_debounced;
    s_touch_last_raw_x = s_touch_debounced ? (uint16_t)lx : 0;
    s_touch_last_raw_y = s_touch_debounced ? (uint16_t)ly : 0;
    s_touch_last_z1 = s_touch_debounced ? lz : 0;

    static bool s_prev_probe_pressed = false;
    static uint64_t s_last_probe_log_ms = 0;
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    if (s_touch_debounced != s_prev_probe_pressed || (now_ms - s_last_probe_log_ms) >= 1500) {
        s_prev_probe_pressed = s_touch_debounced;
        s_last_probe_log_ms = now_ms;
        ESP_LOGI(TAG,
                 "DBG_TOUCH_PROBE raw_pressed=%d filtered=%d debounced=%d x=%d y=%d z=%u streak_p=%u streak_r=%u",
                 filtered_pressed ? 1 : 0,
                 filtered_pressed ? 1 : 0,
                 s_touch_debounced ? 1 : 0,
                 lx,
                 ly,
                 (unsigned)lz,
                 (unsigned)s_touch_press_streak,
                 (unsigned)s_touch_release_streak);
    }

    // Raw SPI bytes are not exposed by esp_lcd_touch; keep legacy fields zeroed.
    s_touch_last_z1_bytes[0] = 0;
    s_touch_last_z1_bytes[1] = 0;
    s_touch_last_z1_bytes[2] = 0;
    s_touch_last_x_bytes[0] = 0;
    s_touch_last_x_bytes[1] = 0;
    s_touch_last_x_bytes[2] = 0;
    s_touch_last_y_bytes[0] = 0;
    s_touch_last_y_bytes[1] = 0;
    s_touch_last_y_bytes[2] = 0;

    if (raw_x) *raw_x = s_touch_debounced ? (uint16_t)lx : 0;
    if (raw_y) *raw_y = s_touch_debounced ? (uint16_t)ly : 0;
    if (z1) *z1 = s_touch_debounced ? lz : 0;
    return s_touch_debounced;
}

bool display_driver_wait_vsync(uint32_t timeout_ms) {
#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        if (!s_p4_render_sem) return false;
        const TickType_t ticks = pdMS_TO_TICKS(timeout_ms ? timeout_ms : 1);
        return xSemaphoreTake(s_p4_render_sem, ticks) == pdTRUE;
    }
#endif
    (void)timeout_ms;
    return true;
}

bool display_driver_p4_begin_frame(uint16_t **out_fb, int *out_w, int *out_h) {
#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        if (out_fb) *out_fb = nullptr;
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        if (!s_p4_display_ready || s_p4_panel == nullptr) return false;
        if (!s_p4_use_framebuffer || s_p4_framebuffer_count < 2) return false;
        if (kP4RenderTrace) {
            const uint64_t ts_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
            ESP_LOGI(TAG, "RENDER begin_frame ts=%llu ms frame=%dx%d",
                     (unsigned long long)ts_ms, P4_PANEL_PHYS_W, P4_PANEL_PHYS_H);
        }
        // Keep exactly one frame in flight to avoid race/tearing artifacts.
        if (s_p4_render_sem) {
            if (xSemaphoreTake(s_p4_render_sem, pdMS_TO_TICKS(120)) != pdTRUE) {
                const TickType_t now = xTaskGetTickCount();
                if ((now - s_p4_last_vsync_warn) > pdMS_TO_TICKS(1000)) {
                    s_p4_last_vsync_warn = now;
                    ESP_LOGW(TAG, "P4 begin_frame timeout: previous frame still in flight");
                }
                return false;
            }
        }
        const int next_fb = (s_p4_framebuffer_index + 1) % s_p4_framebuffer_count;
        uint16_t *fb = s_p4_framebuffers[(size_t)next_fb];
        if (!fb) return false;
        // Keep both framebuffers coherent before partial updates.
        // Without this seed copy, the next backbuffer may contain stale pixels and
        // a later larger dirty region can expose a one-frame flash.
        uint16_t *cur_fb = s_p4_framebuffers[(size_t)s_p4_framebuffer_index];
        if (cur_fb && s_p4_framebuffer_pixels > 0) {
            const size_t bytes = s_p4_framebuffer_pixels * sizeof(uint16_t);
            std::memcpy(fb, cur_fb, bytes);
            (void)esp_cache_msync(
                fb,
                bytes,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
        s_p4_pending_fb = fb;
        s_p4_pending_index = next_fb;
        if (out_fb) *out_fb = fb;
        if (out_w) *out_w = P4_PANEL_PHYS_W;
        if (out_h) *out_h = P4_PANEL_PHYS_H;
        return true;
    }
#endif
    if (out_fb) *out_fb = nullptr;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    return false;
}

bool display_driver_p4_present_frame(void) {
#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        if (!s_p4_display_ready || s_p4_panel == nullptr) return false;
        if (!s_p4_pending_fb || s_p4_pending_index < 0) return false;
        if (kP4RenderTrace) {
            const uint64_t ts_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
            ESP_LOGI(TAG, "RENDER present_frame ts=%llu ms frame=%dx%d",
                     (unsigned long long)ts_ms, P4_PANEL_PHYS_W, P4_PANEL_PHYS_H);
        }
        if (s_p4_framebuffer_pixels > 0) {
            const size_t bytes = s_p4_framebuffer_pixels * sizeof(uint16_t);
            (void)esp_cache_msync(
                s_p4_pending_fb,
                bytes,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
        const esp_err_t err = esp_lcd_panel_draw_bitmap(
            s_p4_panel,
            0,
            0,
            P4_PANEL_PHYS_W,
            P4_PANEL_PHYS_H,
            s_p4_pending_fb);
        if (err == ESP_OK) {
            s_p4_framebuffer_index = s_p4_pending_index;
        } else if (s_p4_render_sem) {
            // If submit failed, release the render token so the pipeline can recover.
            (void)xSemaphoreGive(s_p4_render_sem);
        }
        s_p4_pending_fb = nullptr;
        s_p4_pending_index = -1;
        return err == ESP_OK;
    }
#endif
    return false;
}

bool display_driver_p4_present_frame_region(int x, int y, int w, int h) {
#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        if (!s_p4_display_ready || s_p4_panel == nullptr) return false;
        if (!s_p4_pending_fb || s_p4_pending_index < 0) return false;

        int x1 = std::clamp(x, 0, P4_PANEL_PHYS_W - 1);
        int y1 = std::clamp(y, 0, P4_PANEL_PHYS_H - 1);
        int x2 = std::clamp(x + w - 1, 0, P4_PANEL_PHYS_W - 1);
        int y2 = std::clamp(y + h - 1, 0, P4_PANEL_PHYS_H - 1);
        if (w <= 0 || h <= 0 || x2 < x1 || y2 < y1) {
            return display_driver_p4_present_frame();
        }

        const int copy_w = x2 - x1 + 1;
        const int copy_h = y2 - y1 + 1;
        const size_t copy_pixels = (size_t)copy_w * (size_t)copy_h;
        static std::vector<uint16_t> s_p4_region_buf;
        if (s_p4_region_buf.size() < copy_pixels) {
            s_p4_region_buf.resize(copy_pixels);
        }

        for (int row = 0; row < copy_h; ++row) {
            const size_t src_off = (size_t)(y1 + row) * (size_t)P4_PANEL_PHYS_W + (size_t)x1;
            const size_t dst_off = (size_t)row * (size_t)copy_w;
            std::memcpy(
                s_p4_region_buf.data() + dst_off,
                s_p4_pending_fb + src_off,
                (size_t)copy_w * sizeof(uint16_t));
        }

        (void)esp_cache_msync(
            s_p4_region_buf.data(),
            copy_pixels * sizeof(uint16_t),
            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

        const esp_err_t err = esp_lcd_panel_draw_bitmap(
            s_p4_panel,
            x1,
            y1,
            x2 + 1,
            y2 + 1,
            s_p4_region_buf.data());
        if (err == ESP_OK) {
            s_p4_framebuffer_index = s_p4_pending_index;
        } else if (s_p4_render_sem) {
            (void)xSemaphoreGive(s_p4_render_sem);
        }
        s_p4_pending_fb = nullptr;
        s_p4_pending_index = -1;
        return err == ESP_OK;
    }
#else
    (void)x;
    (void)y;
    (void)w;
    (void)h;
#endif
    return false;
}

void display_driver_p4_cancel_frame(void) {
#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        s_p4_pending_fb = nullptr;
        s_p4_pending_index = -1;
        if (s_p4_render_sem) {
            (void)xSemaphoreGive(s_p4_render_sem);
        }
    }
#endif
}

bool display_driver_touch_probe_raw(uint16_t *raw_x, uint16_t *raw_y, uint16_t *z1) {
    if (!s_touch) {
        if (raw_x) *raw_x = 0;
        if (raw_y) *raw_y = 0;
        if (z1) *z1 = 0;
        return false;
    }

#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        bool locked = false;
        if (s_touch_mutex) {
            locked = (xSemaphoreTake(s_touch_mutex, pdMS_TO_TICKS(10)) == pdTRUE);
        }
        int px = 0;
        int py = 0;
        uint16_t pz = 0;
        const bool raw_pressed = touch_read_once(px, py, pz);
        const bool pressed = raw_pressed;
        if (pressed) {
            px = std::clamp(px, 0, P4_PANEL_PHYS_W - 1);
            py = std::clamp(py, 0, P4_PANEL_PHYS_H - 1);
        }
        if (raw_x) *raw_x = pressed ? (uint16_t)px : 0;
        if (raw_y) *raw_y = pressed ? (uint16_t)py : 0;
        if (z1) *z1 = pressed ? pz : 0;
        if (locked) {
            xSemaphoreGive(s_touch_mutex);
        }
        return pressed;
    }
#endif

    (void)esp_lcd_touch_read_data(s_touch);

    esp_lcd_touch_point_data_t points[1]{};
    uint8_t point_cnt = 0;
    (void)esp_lcd_touch_get_data(s_touch, points, &point_cnt, 1);
    bool pressed = point_cnt > 0;

    int lx = pressed ? (int)points[0].x : 0;
    int ly = pressed ? (int)points[0].y : 0;
    const uint16_t lz = pressed ? points[0].strength : 0;

    if (pressed) {
        static constexpr int RAW_MAX = 4095;
        lx = std::clamp(lx, 0, RAW_MAX);
        ly = std::clamp(ly, 0, RAW_MAX);
    }

    if (pressed && lz < kTouchMinZ1) pressed = false;
    if (pressed) {
        s_touch_press_streak = std::min<uint8_t>(s_touch_press_streak + 1, 255);
        s_touch_release_streak = 0;
        if (s_touch_press_streak >= kTouchDebounceSamples) s_touch_debounced = true;
    } else {
        s_touch_release_streak = std::min<uint8_t>(s_touch_release_streak + 1, 255);
        s_touch_press_streak = 0;
        if (s_touch_release_streak >= kTouchDebounceSamples) s_touch_debounced = false;
    }

    s_touch_last_pressed = s_touch_debounced;
    s_touch_last_raw_x = s_touch_debounced ? (uint16_t)lx : 0;
    s_touch_last_raw_y = s_touch_debounced ? (uint16_t)ly : 0;
    s_touch_last_z1 = s_touch_debounced ? lz : 0;

    if (raw_x) *raw_x = s_touch_debounced ? (uint16_t)lx : 0;
    if (raw_y) *raw_y = s_touch_debounced ? (uint16_t)ly : 0;
    if (z1) *z1 = s_touch_debounced ? lz : 0;
    return s_touch_debounced;
}

void display_driver_get_touch_debug(uint16_t *raw_x, uint16_t *raw_y, uint16_t *z1, bool *pressed) {
    if (raw_x) *raw_x = s_touch_last_raw_x;
    if (raw_y) *raw_y = s_touch_last_raw_y;
    if (z1) *z1 = s_touch_last_z1;
    if (pressed) *pressed = s_touch_last_pressed;
}

void display_driver_get_touch_stats(uint32_t *read_cb_count) {
    if (read_cb_count) *read_cb_count = s_touch_read_cb_count;
}

void display_driver_get_touch_last_bytes(uint8_t out_z1[3], uint8_t out_x[3], uint8_t out_y[3]) {
    if (out_z1) {
        out_z1[0] = s_touch_last_z1_bytes[0];
        out_z1[1] = s_touch_last_z1_bytes[1];
        out_z1[2] = s_touch_last_z1_bytes[2];
    }
    if (out_x) {
        out_x[0] = s_touch_last_x_bytes[0];
        out_x[1] = s_touch_last_x_bytes[1];
        out_x[2] = s_touch_last_x_bytes[2];
    }
    if (out_y) {
        out_y[0] = s_touch_last_y_bytes[0];
        out_y[1] = s_touch_last_y_bytes[1];
        out_y[2] = s_touch_last_y_bytes[2];
    }
}

void display_driver_get_touch_spi(int *mode, int *clock_hz) {
    if (mode) *mode = s_touch_spi_mode;
    if (clock_hz) *clock_hz = s_touch_spi_hz;
}

void display_driver_get_touch_transform(bool *swap_xy, bool *mirror_x, bool *mirror_y) {
    if (swap_xy) *swap_xy = s_touch_swap_xy;
    if (mirror_x) *mirror_x = s_touch_mirror_x;
    if (mirror_y) *mirror_y = s_touch_mirror_y;
}

bool display_driver_set_touch_transform(bool swap_xy, bool mirror_x, bool mirror_y) {
    s_touch_swap_xy = swap_xy;
    s_touch_mirror_x = mirror_x;
    s_touch_mirror_y = mirror_y;
    save_touch_to_nvs();

#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        // GT911 on P4 uses I2C and does not need SPI-style runtime reinit.
        return true;
    }
#endif

    if (!s_touch_io && !s_touch) return true;
    if (touch_deinit() != ESP_OK) return false;
    return touch_init() == ESP_OK;
}

void display_driver_get_touch_calibration(bool *enabled, uint16_t *left, uint16_t *right, uint16_t *top, uint16_t *bottom) {
    if (enabled) *enabled = s_touch_cal_enabled;
    if (left) *left = s_touch_cal_left;
    if (right) *right = s_touch_cal_right;
    if (top) *top = s_touch_cal_top;
    if (bottom) *bottom = s_touch_cal_bottom;
}

bool display_driver_set_touch_calibration(bool enabled, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom) {
    if (right <= left + 10 || bottom <= top + 10) return false;
    // Allow calibration bounds larger than screen size to adjust scale.
    // XPT2046 raw coordinates are 12-bit (0..4095).
    static constexpr uint16_t RAW_MAX = 4095;
    if (left > RAW_MAX || right > RAW_MAX || top > RAW_MAX || bottom > RAW_MAX) return false;
    s_touch_cal_enabled = enabled;
    s_touch_cal_left = left;
    s_touch_cal_right = right;
    s_touch_cal_top = top;
    s_touch_cal_bottom = bottom;
    save_touch_to_nvs();
    return true;
}

void display_driver_reset_touch_calibration(void) {
    s_touch_cal_enabled = false;
    s_touch_cal_left = 0;
    s_touch_cal_right = DISPLAY_WIDTH - 1;
    s_touch_cal_top = 0;
    s_touch_cal_bottom = DISPLAY_HEIGHT - 1;
    save_touch_to_nvs();
}

bool display_driver_is_touch_calibrated(void) {
    return s_touch_cal_enabled || s_touch_affine_enabled || s_touch_grid_enabled;
}

void display_driver_get_touch_affine(bool *enabled, float *a, float *b, float *c, float *d, float *e, float *f) {
    if (enabled) *enabled = s_touch_affine_enabled;
    if (a) *a = s_touch_affine_a;
    if (b) *b = s_touch_affine_b;
    if (c) *c = s_touch_affine_c;
    if (d) *d = s_touch_affine_d;
    if (e) *e = s_touch_affine_e;
    if (f) *f = s_touch_affine_f;
}

bool display_driver_set_touch_affine(bool enabled, float a, float b, float c, float d, float e, float f) {
    s_touch_affine_enabled = enabled;
    s_touch_affine_a = a;
    s_touch_affine_b = b;
    s_touch_affine_c = c;
    s_touch_affine_d = d;
    s_touch_affine_e = e;
    s_touch_affine_f = f;
    save_touch_to_nvs();
    return true;
}

void display_driver_get_touch_grid(bool *enabled, float dx_out[9], float dy_out[9]) {
    if (enabled) *enabled = s_touch_grid_enabled;
    if (dx_out) std::copy(s_touch_grid_dx.begin(), s_touch_grid_dx.end(), dx_out);
    if (dy_out) std::copy(s_touch_grid_dy.begin(), s_touch_grid_dy.end(), dy_out);
}

bool display_driver_set_touch_grid(bool enabled, const float dx[9], const float dy[9]) {
    s_touch_grid_enabled = enabled;
    if (enabled) {
        if (!dx || !dy) return false;
        std::copy(dx, dx + s_touch_grid_dx.size(), s_touch_grid_dx.begin());
        std::copy(dy, dy + s_touch_grid_dy.size(), s_touch_grid_dy.begin());
    } else {
        s_touch_grid_dx.fill(0.0f);
        s_touch_grid_dy.fill(0.0f);
    }
    // Grid and quadratic corrections should not stack.
    if (s_touch_grid_enabled) s_touch_quad_enabled = false;
    save_touch_to_nvs();
    return true;
}

void display_driver_get_touch_pins(int *sclk, int *mosi, int *miso) {
    if (sclk) *sclk = (int)s_touch_sclk;
    if (mosi) *mosi = (int)s_touch_mosi;
    if (miso) *miso = (int)s_touch_miso;
}

bool display_driver_set_touch_pins(int sclk, int mosi, int miso) {
#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        (void)sclk;
        (void)mosi;
        (void)miso;
        // P4 touch is GT911 over fixed I2C pins; SPI pin remap is not applicable.
        return true;
    }
#endif

    if (!GPIO_IS_VALID_GPIO((gpio_num_t)sclk)) return false;
    if (!GPIO_IS_VALID_GPIO((gpio_num_t)mosi)) return false;
    if (!GPIO_IS_VALID_GPIO((gpio_num_t)miso)) return false;

    s_touch_sclk = (gpio_num_t)sclk;
    s_touch_mosi = (gpio_num_t)mosi;
    s_touch_miso = (gpio_num_t)miso;
    save_touch_to_nvs();

    // Caller should reboot to re-init SPI buses with the new pin mapping.
    return true;
}

bool display_driver_set_touch_spi(int mode, int clock_hz) {
#if CONFIG_IDF_TARGET_ESP32P4
    if (board_profile::current_board() == board_profile::BoardId::NewP4) {
        (void)mode;
        (void)clock_hz;
        // P4 touch is GT911 over I2C; SPI mode/frequency controls are not applicable.
        return true;
    }
#endif

    if (mode < 0 || mode > 3) return false;
    if (clock_hz < 50 * 1000 || clock_hz > 2 * 1000 * 1000) return false;

    s_touch_spi_mode = mode;
    s_touch_spi_hz = clock_hz;
    save_touch_to_nvs();

    if (!s_touch_io && !s_touch) return true;

    if (touch_deinit() != ESP_OK) return false;
    return touch_init() == ESP_OK;
}

void display_driver_get_mirror(bool *mirror_x, bool *mirror_y) {
    if (mirror_x) *mirror_x = s_mirror_x;
    if (mirror_y) *mirror_y = s_mirror_y;
}

void display_driver_set_mirror(bool mirror_x, bool mirror_y) {
    s_mirror_x = mirror_x;
    s_mirror_y = mirror_y;
    save_mirror_to_nvs();

    if (s_lcd_spi) {
        (void)lcd_set_rotation_landscape_1();
    }

    if (s_touch) {
        // mirror_y toggles baseline Y inversion (see touch_init).
        (void)esp_lcd_touch_set_mirror_x(s_touch, s_mirror_x);
        (void)esp_lcd_touch_set_mirror_y(s_touch, !s_mirror_y);
    }
}

void display_driver_get_offset(int *x_offset, int *y_offset) {
    if (x_offset) *x_offset = s_lcd_x_offset;
    if (y_offset) *y_offset = s_lcd_y_offset;
}

bool display_driver_set_offset(int x_offset, int y_offset) {
    if (x_offset < -40 || x_offset > 40) return false;
    if (y_offset < -40 || y_offset > 40) return false;
    s_lcd_x_offset = x_offset;
    s_lcd_y_offset = y_offset;
    save_mirror_to_nvs();
    return true;
}
