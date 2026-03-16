#ifndef MAX6675_H
#define MAX6675_H

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>

class MAX6675 {
public:
    MAX6675(gpio_num_t clk, gpio_num_t cs, gpio_num_t do_pin) {
        _sclk = clk;
        _cs = cs;
        _miso = do_pin;

        gpio_config_t io_conf;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << _sclk) | (1ULL << _cs);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);

        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << _miso);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);

        gpio_set_level(_cs, 1);
        gpio_set_level(_sclk, 0); // SPI mode 0 idle low
    }

    float readCelsius(uint16_t *raw_out = nullptr) {
        const uint16_t v = readRaw16();
        if (raw_out) *raw_out = v;

        if (v & 0x4) {
            // Thermocouple open
            return NAN;
        }

        const uint16_t t = (uint16_t)(v >> 3);
        return t * 0.25f;
    }

    uint16_t lastRaw() const { return _last_raw; }

private:
    gpio_num_t _sclk, _cs, _miso;
    uint16_t _last_raw = 0;

    uint8_t readByteSlow() {
        // Compatible with the original (known-working) driver timing:
        // read on SCLK low, then toggle high, with ~1ms delays.
        uint8_t d = 0;
        for (int i = 7; i >= 0; i--) {
            gpio_set_level(_sclk, 0);
            vTaskDelay(pdMS_TO_TICKS(1));
            if (gpio_get_level(_miso)) d |= (uint8_t)(1U << i);
            gpio_set_level(_sclk, 1);
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        return d;
    }

    uint16_t readRaw16() {
        uint16_t v = 0;

        gpio_set_level(_cs, 0);
        vTaskDelay(pdMS_TO_TICKS(1));

        v = (uint16_t)readByteSlow() << 8;
        v |= (uint16_t)readByteSlow();

        gpio_set_level(_cs, 1);

        _last_raw = v;
        return v;
    }
};

#endif
