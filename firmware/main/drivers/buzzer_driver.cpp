#include "drivers/buzzer_driver.h"

#include "driver/ledc.h"
#include "esp_timer.h"
#include "kiln_config/config.h"

namespace {
static bool s_inited = false;
static bool s_active = false;
static uint64_t s_phase_deadline_ms = 0;
static int s_pattern = 0;
static int s_phase = 0;

static constexpr ledc_mode_t kMode = LEDC_LOW_SPEED_MODE;
// Keep buzzer on a dedicated timer to avoid affecting display backlight PWM on NewP4.
static constexpr ledc_timer_t kTimer = LEDC_TIMER_3;
static constexpr ledc_channel_t kChannel = LEDC_CHANNEL_1;
static constexpr ledc_timer_bit_t kDutyRes = LEDC_TIMER_8_BIT;

static void apply_tone(int freq_hz, uint32_t duty) {
    if (!s_inited) return;
    if (freq_hz > 0) {
        (void)ledc_set_freq(kMode, kTimer, (uint32_t)freq_hz);
        (void)ledc_set_duty(kMode, kChannel, duty);
        (void)ledc_update_duty(kMode, kChannel);
    } else {
        (void)ledc_set_duty(kMode, kChannel, 0);
        (void)ledc_update_duty(kMode, kChannel);
    }
}

static int pattern_len(int pattern) {
    if (pattern == 1) return 3;
    if (pattern == 2) return 7;
    return 0;
}

static void phase_desc(int pattern, int phase, int &freq, int &dur_ms, bool &on) {
    freq = 0;
    dur_ms = 0;
    on = false;
    if (pattern == 1) {
        if (phase == 0) { freq = 2400; dur_ms = 110; on = true; }
        if (phase == 1) { freq = 0; dur_ms = 80; on = false; }
        if (phase == 2) { freq = 2400; dur_ms = 110; on = true; }
    } else if (pattern == 2) {
        if (phase == 0) { freq = 1800; dur_ms = 220; on = true; }
        if (phase == 1) { freq = 0; dur_ms = 120; on = false; }
        if (phase == 2) { freq = 2200; dur_ms = 220; on = true; }
        if (phase == 3) { freq = 0; dur_ms = 120; on = false; }
        if (phase == 4) { freq = 2600; dur_ms = 220; on = true; }
        if (phase == 5) { freq = 0; dur_ms = 120; on = false; }
        if (phase == 6) { freq = 3000; dur_ms = 260; on = true; }
    }
}

static void start_pattern(int pattern) {
    if (!s_inited) return;
    s_pattern = pattern;
    s_phase = 0;
    s_active = true;
    s_phase_deadline_ms = 0;
}
}

void buzzer_driver_init(void) {
    if (s_inited) return;
    ledc_timer_config_t timer = {};
    timer.speed_mode = kMode;
    timer.timer_num = kTimer;
    timer.duty_resolution = kDutyRes;
    timer.freq_hz = 2400;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK) return;

    ledc_channel_config_t ch = {};
    ch.speed_mode = kMode;
    ch.channel = kChannel;
    ch.timer_sel = kTimer;
    ch.intr_type = LEDC_INTR_DISABLE;
    ch.gpio_num = (int)BUZZER_PIN;
    ch.duty = 0;
    ch.hpoint = 0;
    if (ledc_channel_config(&ch) != ESP_OK) return;

    s_inited = true;
    apply_tone(0, 0);
}

void buzzer_driver_tick(void) {
    if (!s_inited || !s_active) return;
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    if (s_phase_deadline_ms != 0 && now_ms < s_phase_deadline_ms) return;

    if (s_phase >= pattern_len(s_pattern)) {
        s_active = false;
        s_pattern = 0;
        s_phase = 0;
        s_phase_deadline_ms = 0;
        apply_tone(0, 0);
        return;
    }

    int freq = 0;
    int dur_ms = 0;
    bool on = false;
    phase_desc(s_pattern, s_phase, freq, dur_ms, on);
    if (dur_ms <= 0) {
        s_active = false;
        s_pattern = 0;
        s_phase = 0;
        s_phase_deadline_ms = 0;
        apply_tone(0, 0);
        return;
    }

    if (on) {
        apply_tone(freq, 128);
    } else {
        apply_tone(0, 0);
    }
    s_phase_deadline_ms = now_ms + (uint64_t)dur_ms;
    s_phase++;
}

void buzzer_driver_beep_segment(void) {
    start_pattern(1);
}

void buzzer_driver_beep_complete(void) {
    start_pattern(2);
}

bool buzzer_driver_is_busy(void) {
    return s_active;
}
