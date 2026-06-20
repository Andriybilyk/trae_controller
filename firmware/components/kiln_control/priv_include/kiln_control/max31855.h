#ifndef MAX31855_H
#define MAX31855_H

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include <cmath>
#include <cstdint>

class MAX31855 {
public:
    static constexpr uint32_t RAW_FAULT = 0x00010000U;
    static constexpr uint32_t RAW_OPEN_CIRCUIT = 0x00000001U;
    static constexpr uint32_t RAW_SHORT_TO_GND = 0x00000002U;
    static constexpr uint32_t RAW_SHORT_TO_VCC = 0x00000004U;
    static constexpr uint32_t RAW_FAULT_DETAIL_MASK = RAW_OPEN_CIRCUIT | RAW_SHORT_TO_GND | RAW_SHORT_TO_VCC;

    MAX31855(gpio_num_t clk, gpio_num_t cs, gpio_num_t so_pin) {
        _sclk = clk;
        _cs = cs;
        _miso = so_pin;

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
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);

        gpio_set_level(_cs, 1);
        gpio_set_level(_sclk, 0);
    }

    float readCelsius(uint32_t *raw_out = nullptr) {
        const uint32_t v = readRaw32();
        if (raw_out) *raw_out = v;

        if (hasFault(v)) return NAN;

        int32_t t14 = (int32_t)((v >> 18) & 0x3FFFU);
        if (t14 & 0x2000) t14 |= ~0x3FFF;
        return (float)t14 * 0.25f;
    }

    uint32_t lastRaw() const { return _last_raw; }
    static bool hasFault(uint32_t raw) { return (raw & RAW_FAULT) != 0; }
    static uint32_t faultDetailBits(uint32_t raw) { return hasFault(raw) ? (raw & RAW_FAULT_DETAIL_MASK) : 0U; }
    static bool isOpenCircuit(uint32_t raw) { return (faultDetailBits(raw) & RAW_OPEN_CIRCUIT) != 0; }
    static bool isShortToGnd(uint32_t raw) { return (faultDetailBits(raw) & RAW_SHORT_TO_GND) != 0; }
    static bool isShortToVcc(uint32_t raw) { return (faultDetailBits(raw) & RAW_SHORT_TO_VCC) != 0; }

private:
    gpio_num_t _sclk, _cs, _miso;
    uint32_t _last_raw = 0;

    static inline void delay_edge() { esp_rom_delay_us(10); }

    uint8_t readByteSlow() {
        uint8_t d = 0;
        for (int i = 7; i >= 0; i--) {
            gpio_set_level(_sclk, 1);
            delay_edge();
            if (gpio_get_level(_miso)) d |= (uint8_t)(1U << i);
            gpio_set_level(_sclk, 0);
            delay_edge();
        }
        return d;
    }

    uint32_t readRaw32() {
        uint32_t v = 0;
        gpio_set_level(_cs, 0);
        delay_edge();

        v = (uint32_t)readByteSlow() << 24;
        v |= (uint32_t)readByteSlow() << 16;
        v |= (uint32_t)readByteSlow() << 8;
        v |= (uint32_t)readByteSlow();

        gpio_set_level(_cs, 1);
        _last_raw = v;
        return v;
    }
};

#endif
