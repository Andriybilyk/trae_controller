#include "display_driver.h"

#include "config.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DISPLAY";

static constexpr spi_host_device_t LCD_HOST = SPI2_HOST;

static spi_device_handle_t s_lcd_spi = nullptr;
static spi_device_handle_t s_touch_spi = nullptr;

static constexpr int DISPLAY_WIDTH = 480;  // LVGL landscape
static constexpr int DISPLAY_HEIGHT = 320; // LVGL landscape

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
    return spi_device_polling_transmit(s_lcd_spi, &t);
}

static esp_err_t lcd_cmd(uint8_t cmd) { return lcd_tx(&cmd, 1, false); }

static esp_err_t lcd_data(const void *data, size_t len_bytes) { return lcd_tx(data, len_bytes, true); }

static esp_err_t lcd_write_u16be(uint16_t value) {
    uint8_t b[2] = {(uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return lcd_data(b, sizeof(b));
}

static esp_err_t lcd_set_addr_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    const uint16_t x2 = x + w - 1;
    const uint16_t y2 = y + h - 1;

    ESP_RETURN_ON_ERROR(lcd_cmd(0x2A), TAG, "CASET");
    ESP_RETURN_ON_ERROR(lcd_write_u16be(x), TAG, "CASET x1");
    ESP_RETURN_ON_ERROR(lcd_write_u16be(x2), TAG, "CASET x2");

    ESP_RETURN_ON_ERROR(lcd_cmd(0x2B), TAG, "PASET");
    ESP_RETURN_ON_ERROR(lcd_write_u16be(y), TAG, "PASET y1");
    ESP_RETURN_ON_ERROR(lcd_write_u16be(y2), TAG, "PASET y2");

    return lcd_cmd(0x2C); // RAMWR
}

static esp_err_t lcd_set_rotation_landscape_1(void) {
    // Based on common MADCTL values (RGB order, MV set).
    // If colors are swapped, toggle the BGR bit (0x08).
    uint8_t madctl = 0x20; // MV
    ESP_RETURN_ON_ERROR(lcd_cmd(0x36), TAG, "MADCTL cmd");
    return lcd_data(&madctl, 1);
}

static esp_err_t st7796_init_sequence(void) {
    // Init sequence adapted from LovyanGFX Panel_ST7796 defaults.
    const auto tx = [](uint8_t cmd, std::initializer_list<uint8_t> data, int delay_ms = 0) -> esp_err_t {
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
    return ESP_OK;
}

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
    gpio_set_level(TFT_BL, 0);
}

static esp_err_t display_spi_init(void) {
    spi_bus_config_t buscfg{};
    buscfg.sclk_io_num = TFT_SCLK;
    buscfg.mosi_io_num = TFT_MOSI;
    buscfg.miso_io_num = TFT_MISO;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = DISPLAY_WIDTH * 10 * 2 + 8;

    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi_bus_initialize");

    spi_device_interface_config_t devcfg{};
    devcfg.clock_speed_hz = 20 * 1000 * 1000;
    devcfg.mode = 0;
    devcfg.spics_io_num = TFT_CS;
    devcfg.queue_size = 7;
    devcfg.pre_cb = lcd_spi_pre_transfer_cb;
    devcfg.flags = SPI_DEVICE_HALFDUPLEX;

    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_HOST, &devcfg, &s_lcd_spi), TAG, "spi_bus_add_device lcd");

    spi_device_interface_config_t touchcfg{};
    touchcfg.clock_speed_hz = 1 * 1000 * 1000;
    touchcfg.mode = 0;
    touchcfg.spics_io_num = TOUCH_CS;
    touchcfg.queue_size = 1;
    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_HOST, &touchcfg, &s_touch_spi), TAG, "spi_bus_add_device touch");

    return ESP_OK;
}

static void display_reset(void) {
    gpio_set_level(TFT_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

void display_driver_init(void) {
    display_gpio_init();
    ESP_ERROR_CHECK(display_spi_init());
    display_reset();
    ESP_ERROR_CHECK(st7796_init_sequence());

    gpio_set_level(TFT_BL, 1);
    ESP_LOGI(TAG, "Display initialized");
}

void display_driver_lvgl_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    const uint32_t w = (area->x2 - area->x1 + 1);
    const uint32_t h = (area->y2 - area->y1 + 1);

    // LVGL uses 0..DISPLAY_WIDTH-1 / 0..DISPLAY_HEIGHT-1 in landscape.
    // With MV rotation configured, these map directly to controller coords.
    if (lcd_set_addr_window(area->x1, area->y1, w, h) == ESP_OK) {
        const size_t bytes = w * h * sizeof(lv_color_t);
        (void)lcd_data(color_p, bytes);
    }

    lv_disp_flush_ready(disp);
}

static uint16_t xpt2046_read12(uint8_t cmd) {
    if (!s_touch_spi) return 0;

    uint8_t tx[3] = {cmd, 0x00, 0x00};
    uint8_t rx[3] = {0};

    spi_transaction_t t{};
    t.length = 3 * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    if (spi_device_polling_transmit(s_touch_spi, &t) != ESP_OK) return 0;

    const uint16_t v = (uint16_t)((rx[1] << 8) | rx[2]);
    return (v >> 3) & 0x0FFF;
}

static bool touch_read_raw(uint16_t &raw_x, uint16_t &raw_y) {
    // Basic pressure check (Z1). Threshold is empirical.
    const uint16_t z1 = xpt2046_read12(0xB0);
    if (z1 < 50) return false;

    // Median of 3 to reduce noise.
    std::array<uint16_t, 3> xs{ xpt2046_read12(0xD0), xpt2046_read12(0xD0), xpt2046_read12(0xD0) };
    std::array<uint16_t, 3> ys{ xpt2046_read12(0x90), xpt2046_read12(0x90), xpt2046_read12(0x90) };
    std::sort(xs.begin(), xs.end());
    std::sort(ys.begin(), ys.end());
    raw_x = xs[1];
    raw_y = ys[1];
    return true;
}

void display_driver_lvgl_touch_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    (void)indev_driver;

    uint16_t raw_x = 0;
    uint16_t raw_y = 0;
    if (!touch_read_raw(raw_x, raw_y)) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    // Map raw 0..4095 to LVGL landscape coordinates.
    // Note: You may need to tweak swap/invert depending on your panel wiring.
    int x = (int)((raw_y * (uint32_t)DISPLAY_WIDTH) / 4095);
    int y = (int)(DISPLAY_HEIGHT - 1 - ((raw_x * (uint32_t)DISPLAY_HEIGHT) / 4095));

    x = std::clamp(x, 0, DISPLAY_WIDTH - 1);
    y = std::clamp(y, 0, DISPLAY_HEIGHT - 1);

    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
}
