#ifndef MAX6675_H
#define MAX6675_H

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>

extern volatile uint16_t g_raw_value;

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
        gpio_config(&io_conf);

        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << _miso);
        gpio_config(&io_conf);

        gpio_set_level(_cs, 1);
    }

    float readCelsius() {
        uint16_t v;

        gpio_set_level(_cs, 0);
        vTaskDelay(pdMS_TO_TICKS(1));

        v = spiread();
        v <<= 8;
        v |= spiread();

        g_raw_value = v; // Store raw value

        gpio_set_level(_cs, 1);

        if (v & 0x4) {
            // Thermocouple open
            return NAN;
        }

        v >>= 3;
        return v * 0.25;
    }

private:
    gpio_num_t _sclk, _cs, _miso;

    uint8_t spiread() {
        uint8_t d = 0;
        for (int i = 7; i >= 0; i--) {
            gpio_set_level(_sclk, 0);
            vTaskDelay(pdMS_TO_TICKS(1));
            if (gpio_get_level(_miso)) {
                d |= (1 << i);
            }
            gpio_set_level(_sclk, 1);
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        return d;
    }
};

#endif
