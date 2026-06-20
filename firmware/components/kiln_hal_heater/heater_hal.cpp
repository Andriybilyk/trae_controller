#include "kiln_hal_heater/heater_hal.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kiln_config/config.h"

static const char *TAG = "HEATER_HAL";
static bool s_inited = false;
static uint64_t s_mask = 0;
static bool s_display_ready = false;
static bool s_safety_level_known = false;
static bool s_safety_level = false;
static bool s_ssr_level_known = false;
static bool s_ssr_level = false;

static inline bool gpio_is_valid_out(gpio_num_t pin) {
    return GPIO_IS_VALID_OUTPUT_GPIO(pin);
}

static inline bool add_gpio_to_mask_if_valid(uint64_t *mask, int pin_value) {
    if (!mask) return false;
    if (pin_value < 0 || pin_value >= 64) return false;
    const gpio_num_t pin = static_cast<gpio_num_t>(pin_value);
    if (!gpio_is_valid_out(pin)) return false;
    *mask |= (1ULL << static_cast<uint32_t>(pin_value));
    return true;
}

void heater_hal_init(void) {
    if (s_inited) return;
    s_inited = true;

#if CONFIG_TC_BOARD_NEW_P4
    // Display bring-up isolation on NewP4:
    // keep relay pin in high-impedance input mode with pulldown until display is fully ready.
    s_mask = 0;
    s_display_ready = false;
    if (GPIO_IS_VALID_GPIO(SAFETY_RELAY_PIN)) {
        gpio_config_t io_safe = {};
        io_safe.pin_bit_mask = (1ULL << static_cast<uint32_t>(SAFETY_RELAY_PIN));
        io_safe.mode = GPIO_MODE_INPUT;
        io_safe.pull_down_en = GPIO_PULLDOWN_ENABLE;
        io_safe.pull_up_en = GPIO_PULLUP_DISABLE;
        io_safe.intr_type = GPIO_INTR_DISABLE;
        (void)gpio_config(&io_safe);
    }
    return;
#endif

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT;
    s_mask = 0;
    (void)add_gpio_to_mask_if_valid(&s_mask, static_cast<int>(SSR_ZONE1_PIN));
    (void)add_gpio_to_mask_if_valid(&s_mask, static_cast<int>(SAFETY_RELAY_PIN));
    io_conf.pin_bit_mask = s_mask;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    if (io_conf.pin_bit_mask != 0) {
        if (GPIO_IS_VALID_GPIO(SSR_ZONE1_PIN)) (void)gpio_reset_pin(SSR_ZONE1_PIN);
        if (GPIO_IS_VALID_GPIO(SAFETY_RELAY_PIN)) (void)gpio_reset_pin(SAFETY_RELAY_PIN);
        (void)gpio_config(&io_conf);
        if (gpio_is_valid_out((gpio_num_t)SSR_ZONE1_PIN)) {
            (void)gpio_set_drive_capability((gpio_num_t)SSR_ZONE1_PIN, GPIO_DRIVE_CAP_3);
        }
        if (gpio_is_valid_out((gpio_num_t)SAFETY_RELAY_PIN)) {
            (void)gpio_set_drive_capability((gpio_num_t)SAFETY_RELAY_PIN, GPIO_DRIVE_CAP_3);
        }
    }

    ESP_LOGI(TAG, "Init: SSR pin=%d active_low=%d safety pin=%d mask=0x%08" PRIx32 "%08" PRIx32,
             (int)SSR_ZONE1_PIN, (int)SSR_ZONE1_ACTIVE_LOW, (int)SAFETY_RELAY_PIN,
             (uint32_t)(s_mask >> 32), (uint32_t)(s_mask & 0xFFFFFFFFu));
    heater_hal_all_off();
#if SSR_BOOT_DIAG_PULSE_MS > 0
    if (gpio_is_valid_out((gpio_num_t)SSR_ZONE1_PIN)) {
        ESP_LOGW(TAG, "Boot diag pulse on SSR pin %d for %d ms", (int)SSR_ZONE1_PIN, (int)SSR_BOOT_DIAG_PULSE_MS);
        heater_hal_set_safety_relay(false);
        heater_hal_set_ssr(true);
        vTaskDelay(pdMS_TO_TICKS(SSR_BOOT_DIAG_PULSE_MS));
        heater_hal_set_ssr(false);
        heater_hal_set_safety_relay(false);
    }
#endif
}

void heater_hal_set_ssr(bool on) {
#if CONFIG_TC_BOARD_NEW_P4
    (void)on;
    return;
#endif
    if (!gpio_is_valid_out((gpio_num_t)SSR_ZONE1_PIN)) return;
    const int level = on ? (SSR_ZONE1_ACTIVE_LOW ? 0 : 1) : (SSR_ZONE1_ACTIVE_LOW ? 1 : 0);
    if (s_ssr_level_known && s_ssr_level == on) return;
    (void)gpio_set_level((gpio_num_t)SSR_ZONE1_PIN, level);
    const int rb = gpio_get_level((gpio_num_t)SSR_ZONE1_PIN);
    s_ssr_level = on;
    s_ssr_level_known = true;
    ESP_LOGI(TAG, "SSR -> %s (pin=%d level=%d rb=%d active_low=%d)", on ? "ON" : "OFF",
             (int)SSR_ZONE1_PIN, level, rb, (int)SSR_ZONE1_ACTIVE_LOW);
}

void heater_hal_set_safety_relay(bool on) {
#if CONFIG_TC_BOARD_NEW_P4
    if (!s_display_ready) return;
    if (!gpio_is_valid_out((gpio_num_t)SAFETY_RELAY_PIN)) return;
    if (s_safety_level_known && s_safety_level == on) return;
    (void)gpio_set_level((gpio_num_t)SAFETY_RELAY_PIN, on ? 1 : 0);
    s_safety_level = on;
    s_safety_level_known = true;
    ESP_LOGI(TAG, "Safety relay -> %s", on ? "ON" : "OFF");
#else
    if (!gpio_is_valid_out((gpio_num_t)SAFETY_RELAY_PIN)) return;
    if (s_safety_level_known && s_safety_level == on) return;
    (void)gpio_set_level((gpio_num_t)SAFETY_RELAY_PIN, on ? 1 : 0);
    s_safety_level = on;
    s_safety_level_known = true;
    ESP_LOGI(TAG, "Safety relay -> %s", on ? "ON" : "OFF");
#endif
}

void heater_hal_all_off(void) {
    heater_hal_set_ssr(false);
    heater_hal_set_safety_relay(false);
}

void heater_hal_set_display_ready(bool ready) {
#if CONFIG_TC_BOARD_NEW_P4
    s_display_ready = ready;
    if (!GPIO_IS_VALID_GPIO(SAFETY_RELAY_PIN)) return;
    if (!ready) {
        gpio_config_t io_safe = {};
        io_safe.pin_bit_mask = (1ULL << static_cast<uint32_t>(SAFETY_RELAY_PIN));
        io_safe.mode = GPIO_MODE_INPUT;
        io_safe.pull_down_en = GPIO_PULLDOWN_ENABLE;
        io_safe.pull_up_en = GPIO_PULLUP_DISABLE;
        io_safe.intr_type = GPIO_INTR_DISABLE;
        (void)gpio_config(&io_safe);
        s_safety_level_known = false;
        return;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(SAFETY_RELAY_PIN)) return;
    gpio_config_t io_out = {};
    io_out.pin_bit_mask = (1ULL << static_cast<uint32_t>(SAFETY_RELAY_PIN));
    io_out.mode = GPIO_MODE_OUTPUT;
    io_out.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_out.pull_up_en = GPIO_PULLUP_DISABLE;
    io_out.intr_type = GPIO_INTR_DISABLE;
    (void)gpio_config(&io_out);
    // Keep relay inactive immediately after enabling output.
    (void)gpio_set_level(SAFETY_RELAY_PIN, 0);
    s_safety_level = false;
    s_safety_level_known = true;
#else
    (void)ready;
#endif
}
