#include "drivers/rtc_ds3231.h"

#include "kiln_config/config.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#include <cstring>
#include <sys/time.h>

static const char *TAG = "RTC_DS3231";
static bool s_inited = false;
static i2c_master_bus_handle_t s_i2c_bus = nullptr;
static i2c_master_dev_handle_t s_rtc_dev = nullptr;

static uint8_t to_bcd(int v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int from_bcd(uint8_t v) {
    return ((v >> 4) * 10) + (v & 0x0F);
}

static bool i2c_write_reg(uint8_t reg, const uint8_t *data, size_t len) {
    uint8_t buf[16];
    if (len + 1 > sizeof(buf)) return false;
    buf[0] = reg;
    if (len > 0 && data) std::memcpy(&buf[1], data, len);
    const esp_err_t err = i2c_master_transmit(s_rtc_dev, buf, len + 1, 200);
    return err == ESP_OK;
}

static bool i2c_read_reg(uint8_t reg, uint8_t *data, size_t len) {
    if (!data || len == 0) return false;
    const esp_err_t err = i2c_master_transmit_receive(s_rtc_dev, &reg, 1, data, len, 200);
    return err == ESP_OK;
}

bool rtc_ds3231_init(void) {
    if (s_inited) return true;
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = (i2c_port_num_t)RTC_I2C_PORT;
    bus_cfg.sda_io_num = RTC_I2C_SDA;
    bus_cfg.scl_io_num = RTC_I2C_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.intr_priority = 0;
    bus_cfg.trans_queue_depth = 0;
    bus_cfg.flags.enable_internal_pullup = 1;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) return false;

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = RTC_DS3231_ADDR;
    dev_cfg.scl_speed_hz = RTC_I2C_FREQ_HZ;
    dev_cfg.scl_wait_us = 0;
    dev_cfg.flags.disable_ack_check = 0;
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_rtc_dev);
    if (err != ESP_OK) {
        (void)i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = nullptr;
        return false;
    }

    s_inited = true;
    ESP_LOGI(TAG, "RTC init OK (SDA=%d, SCL=%d)", (int)RTC_I2C_SDA, (int)RTC_I2C_SCL);
    return true;
}

bool rtc_ds3231_is_clock_valid(bool *clock_valid) {
    if (!rtc_ds3231_init()) return false;
    uint8_t status = 0;
    if (!i2c_read_reg(0x0F, &status, 1)) return false;
    if (clock_valid) *clock_valid = (status & 0x80) == 0;
    return true;
}

bool rtc_ds3231_read_time(struct tm *out_tm, bool *clock_valid) {
    if (!out_tm) return false;
    if (!rtc_ds3231_init()) return false;

    uint8_t regs[7] = {0};
    if (!i2c_read_reg(0x00, regs, sizeof(regs))) return false;

    std::memset(out_tm, 0, sizeof(struct tm));
    out_tm->tm_sec = from_bcd(regs[0] & 0x7F);
    out_tm->tm_min = from_bcd(regs[1] & 0x7F);
    out_tm->tm_hour = from_bcd(regs[2] & 0x3F);
    out_tm->tm_mday = from_bcd(regs[4] & 0x3F);
    out_tm->tm_mon = from_bcd(regs[5] & 0x1F) - 1;
    out_tm->tm_year = from_bcd(regs[6]) + 100;
    out_tm->tm_isdst = -1;

    bool valid = false;
    if (!rtc_ds3231_is_clock_valid(&valid)) return false;
    if (clock_valid) *clock_valid = valid;
    return true;
}

bool rtc_ds3231_set_time(const struct tm *tm_in) {
    if (!tm_in) return false;
    if (!rtc_ds3231_init()) return false;
    if (tm_in->tm_year < 100 || tm_in->tm_year > 199) return false;

    uint8_t regs[7] = {0};
    regs[0] = to_bcd(tm_in->tm_sec);
    regs[1] = to_bcd(tm_in->tm_min);
    regs[2] = to_bcd(tm_in->tm_hour);
    regs[3] = to_bcd(tm_in->tm_wday <= 0 ? 1 : tm_in->tm_wday);
    regs[4] = to_bcd(tm_in->tm_mday);
    regs[5] = to_bcd(tm_in->tm_mon + 1);
    regs[6] = to_bcd(tm_in->tm_year - 100);
    if (!i2c_write_reg(0x00, regs, sizeof(regs))) return false;

    uint8_t status = 0;
    if (!i2c_read_reg(0x0F, &status, 1)) return false;
    status &= (uint8_t)~0x80;
    if (!i2c_write_reg(0x0F, &status, 1)) return false;
    return true;
}

bool rtc_ds3231_apply_time_to_system(void) {
    struct tm tmv = {};
    bool valid = false;
    if (!rtc_ds3231_read_time(&tmv, &valid)) return false;
    if (!valid) return false;
    const time_t ts = mktime(&tmv);
    if (ts < 1577836800) return false;
    struct timeval tv = {};
    tv.tv_sec = ts;
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) return false;
    ESP_LOGI(TAG, "System time restored from RTC");
    return true;
}

bool rtc_ds3231_sync_from_system_time(void) {
    if (!rtc_ds3231_init()) return false;
    time_t now = 0;
    time(&now);
    if (now < 1577836800) return false;
    struct tm tmv = {};
    localtime_r(&now, &tmv);
    if (!rtc_ds3231_set_time(&tmv)) return false;
    ESP_LOGI(TAG, "RTC synced from system time");
    return true;
}
