#ifndef MAX6675_H
#define MAX6675_H

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>

class MAX6675 {
public:
    MAX6675(gpio_num_t sclk, gpio_num_t cs, gpio_num_t miso) {
        _sclk = sclk;
        _cs = cs;
        _miso = miso;

        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << _sclk) | (1ULL << _cs);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
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

        gpio_set_level(_cs, 1);

        if (v & 0x4) {
            // No thermocouple attached!
            return NAN;
        }

        v >>= 3;
        return v * 0.25;
    }

private:
    uint8_t spiread() {
        int i;
        uint8_t d = 0;

        for (i = 7; i >= 0; i--) {
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

    gpio_num_t _sclk, _cs, _miso;
};

#endif
