#include "kiln_hal_heater/heater_hal.h"

#include "driver/gpio.h"
#include "kiln_config/config.h"

static bool s_inited = false;

void heater_hal_init(void) {
    if (s_inited) return;
    s_inited = true;

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << SSR_ZONE1_PIN) | (1ULL << SAFETY_RELAY_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    (void)gpio_config(&io_conf);

    heater_hal_all_off();
}

void heater_hal_set_ssr(bool on) {
    (void)gpio_set_level((gpio_num_t)SSR_ZONE1_PIN, on ? 1 : 0);
}

void heater_hal_set_safety_relay(bool on) {
    (void)gpio_set_level((gpio_num_t)SAFETY_RELAY_PIN, on ? 1 : 0);
}

void heater_hal_all_off(void) {
    heater_hal_set_ssr(false);
    heater_hal_set_safety_relay(false);
}

