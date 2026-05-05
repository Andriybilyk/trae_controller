#include "drivers/fan_driver.h"

#include "esp_log.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "kiln_config/config.h"

static const char *TAG = "FAN";

static bool s_manual = false;
static uint8_t s_power_percent = 0;
static bool s_auto_enabled = true;
static float s_auto_temp_min_c = 45.0f;
static float s_auto_temp_max_c = 280.0f;
static uint8_t s_auto_power_min_percent = 0;
static uint8_t s_auto_power_max_percent = 100;
static uint8_t s_effective_power_percent = 0;
static float s_source_temp_c = 0.0f;
static bool s_inited = false;
static SemaphoreHandle_t s_mutex = nullptr;
static constexpr uint8_t kFiringMinPowerPercent = 60;

static constexpr ledc_mode_t kMode = LEDC_LOW_SPEED_MODE;
static constexpr ledc_timer_t kTimer = LEDC_TIMER_0;
static constexpr ledc_channel_t kChannel = LEDC_CHANNEL_0;
static constexpr ledc_timer_bit_t kDutyRes = LEDC_TIMER_10_BIT; // 0..1023
static constexpr uint32_t kPwmHz = 25000;                       // keep it above audible range

static uint32_t duty_from_percent(uint8_t percent) {
    if (percent >= 100) return (1u << kDutyRes) - 1u;
    return (uint32_t)percent * (((1u << kDutyRes) - 1u)) / 100u;
}

static void lock_state() {
    if (s_mutex) xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}

static void unlock_state() {
    if (s_mutex) xSemaphoreGiveRecursive(s_mutex);
}

static void apply_output(void) {
    if (!s_inited) return;
    const uint8_t out = s_effective_power_percent;
    uint32_t duty = (out > 0) ? duty_from_percent(out) : 0;
#if FAN_ACTIVE_LOW
    const uint32_t max_duty = (1u << kDutyRes) - 1u;
    duty = max_duty - duty;
#endif
    (void)ledc_set_duty(kMode, kChannel, duty);
    (void)ledc_update_duty(kMode, kChannel);
}

static uint8_t auto_power_for_temp(float temp_c) {
    if (!s_auto_enabled) return 0;
    if (s_auto_temp_max_c <= s_auto_temp_min_c) {
        return s_auto_power_max_percent;
    }
    if (temp_c <= s_auto_temp_min_c) return s_auto_power_min_percent;
    if (temp_c >= s_auto_temp_max_c) return s_auto_power_max_percent;

    const float k = (temp_c - s_auto_temp_min_c) / (s_auto_temp_max_c - s_auto_temp_min_c);
    const float p = (float)s_auto_power_min_percent +
                    k * (float)((int)s_auto_power_max_percent - (int)s_auto_power_min_percent);
    int out = (int)(p + 0.5f);
    if (out < 0) out = 0;
    if (out > 100) out = 100;
    return (uint8_t)out;
}

void fan_driver_init(void) {
    if (!s_mutex) s_mutex = xSemaphoreCreateRecursiveMutex();
    lock_state();
    if (s_inited) {
        unlock_state();
        return;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(FAN_PIN)) {
        ESP_LOGI(TAG, "Skipped (invalid GPIO for current board profile)");
        unlock_state();
        return;
    }

    ledc_timer_config_t timer = {};
    timer.speed_mode = kMode;
    timer.timer_num = kTimer;
    timer.duty_resolution = kDutyRes;
    timer.freq_hz = kPwmHz;
    timer.clk_cfg = LEDC_AUTO_CLK;

    const esp_err_t t_ok = ledc_timer_config(&timer);
    if (t_ok != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(t_ok));
        unlock_state();
        return;
    }

    ledc_channel_config_t ch = {};
    ch.speed_mode = kMode;
    ch.channel = kChannel;
    ch.timer_sel = kTimer;
    ch.intr_type = LEDC_INTR_DISABLE;
    ch.gpio_num = (int)FAN_PIN;
    ch.duty = 0;
    ch.hpoint = 0;

    const esp_err_t c_ok = ledc_channel_config(&ch);
    if (c_ok != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(c_ok));
        unlock_state();
        return;
    }

    s_inited = true;
    s_manual = false;
    s_power_percent = 0;
    s_effective_power_percent = 0;
    apply_output();

    ESP_LOGI(TAG, "Init OK (GPIO=%d, %u Hz)", (int)FAN_PIN, (unsigned)kPwmHz);
    unlock_state();
}

void fan_driver_set_manual(bool enabled) {
    lock_state();
    s_manual = enabled;
    s_effective_power_percent = s_manual ? s_power_percent : 0;
    apply_output();
    unlock_state();
}

void fan_driver_set_power_percent(uint8_t percent) {
    lock_state();
    if (percent > 100) percent = 100;
    s_power_percent = percent;
    if (s_manual) s_effective_power_percent = s_power_percent;
    apply_output();
    unlock_state();
}

bool fan_driver_get_manual(void) {
    lock_state();
    const bool v = s_manual;
    unlock_state();
    return v;
}
uint8_t fan_driver_get_power_percent(void) {
    lock_state();
    const uint8_t v = s_power_percent;
    unlock_state();
    return v;
}

void fan_driver_set_auto_enabled(bool enabled) {
    lock_state();
    s_auto_enabled = enabled;
    unlock_state();
}

bool fan_driver_get_auto_enabled(void) {
    lock_state();
    const bool v = s_auto_enabled;
    unlock_state();
    return v;
}

void fan_driver_set_auto_curve(float temp_min_c, float temp_max_c, uint8_t power_min_percent, uint8_t power_max_percent) {
    lock_state();
    if (temp_min_c < -50.0f) temp_min_c = -50.0f;
    if (temp_max_c > 1300.0f) temp_max_c = 1300.0f;
    if (temp_max_c < temp_min_c + 1.0f) temp_max_c = temp_min_c + 1.0f;

    if (power_min_percent > 100) power_min_percent = 100;
    if (power_max_percent > 100) power_max_percent = 100;
    if (power_max_percent < power_min_percent) power_max_percent = power_min_percent;

    s_auto_temp_min_c = temp_min_c;
    s_auto_temp_max_c = temp_max_c;
    s_auto_power_min_percent = power_min_percent;
    s_auto_power_max_percent = power_max_percent;
    unlock_state();
}

void fan_driver_get_auto_curve(float *temp_min_c, float *temp_max_c, uint8_t *power_min_percent, uint8_t *power_max_percent) {
    lock_state();
    if (temp_min_c) *temp_min_c = s_auto_temp_min_c;
    if (temp_max_c) *temp_max_c = s_auto_temp_max_c;
    if (power_min_percent) *power_min_percent = s_auto_power_min_percent;
    if (power_max_percent) *power_max_percent = s_auto_power_max_percent;
    unlock_state();
}

void fan_driver_update_from_temperature(float temp_c, bool is_firing) {
    lock_state();
    s_source_temp_c = temp_c;
    if (s_manual) {
        s_effective_power_percent = s_power_percent;
    } else {
        uint8_t auto_power = auto_power_for_temp(temp_c);
        if (is_firing && auto_power < kFiringMinPowerPercent) {
            auto_power = kFiringMinPowerPercent;
        }
        s_effective_power_percent = auto_power;
    }
    apply_output();
    unlock_state();
}

uint8_t fan_driver_get_effective_power_percent(void) {
    lock_state();
    const uint8_t v = s_effective_power_percent;
    unlock_state();
    return v;
}

float fan_driver_get_source_temp_c(void) {
    lock_state();
    const float v = s_source_temp_c;
    unlock_state();
    return v;
}
