#include "drivers/display_driver.h"

#include "kiln_config/config.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <vector>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "DISPLAY";

static constexpr spi_host_device_t LCD_HOST = SPI2_HOST;
static constexpr spi_host_device_t TOUCH_HOST = SPI3_HOST;

static spi_device_handle_t s_lcd_spi = nullptr;
static spi_host_device_t s_touch_host = LCD_HOST;

// ST7796 expects RGB565 pixels MSB-first. Slint's software renderer produces native-endian u16
// pixels (little-endian on ESP32-S3), so we byte-swap before pushing to the panel.
static bool s_lcd_swap_bytes = true;

static constexpr int DISPLAY_WIDTH = 480;  // LVGL landscape
static constexpr int DISPLAY_HEIGHT = 320; // LVGL landscape
static constexpr int LCD_X_OFFSET = 0;
static constexpr int LCD_Y_OFFSET = 0;
static constexpr size_t LCD_SPI_CHUNK_BYTES =
    (size_t)DISPLAY_WIDTH * (size_t)((TFT_SPI_DMA_LINES > 0) ? TFT_SPI_DMA_LINES : 1) * 2U;

static bool s_mirror_x = false;
static bool s_mirror_y = false;
static int s_lcd_x_offset = 0;
static int s_lcd_y_offset = 0;

static int s_touch_spi_mode = 0;
static int s_touch_spi_hz = TOUCH_SPI_CLOCK_HZ;

static bool s_touch_swap_xy = false;
static bool s_touch_mirror_x = false;
static bool s_touch_mirror_y = false;

static bool s_touch_cal_enabled = false;
static uint16_t s_touch_cal_left = 0;
static uint16_t s_touch_cal_right = DISPLAY_WIDTH - 1;
static uint16_t s_touch_cal_top = 0;
static uint16_t s_touch_cal_bottom = DISPLAY_HEIGHT - 1;

static bool s_touch_affine_enabled = false;
static float s_touch_affine_a = 1.0f;
static float s_touch_affine_b = 0.0f;
static float s_touch_affine_c = 0.0f;
static float s_touch_affine_d = 0.0f;
static float s_touch_affine_e = 1.0f;
static float s_touch_affine_f = 0.0f;

static bool s_touch_grid_enabled = false;
static std::array<float, 9> s_touch_grid_dx{};
static std::array<float, 9> s_touch_grid_dy{};

// Optional quadratic correction for Y to compensate nonlinearity.
// y' = a*y^2 + b*y + c
static bool s_touch_quad_enabled = false;
static float s_touch_quad_a = 0.0044125721f;
static float s_touch_quad_b = -0.65727115f;
static float s_touch_quad_c = 60.641743f;

static gpio_num_t s_touch_sclk = TOUCH_SCLK;
static gpio_num_t s_touch_mosi = TOUCH_MOSI;
static gpio_num_t s_touch_miso = TOUCH_MISO;

static esp_lcd_panel_io_handle_t s_touch_io = nullptr;
static esp_lcd_touch_handle_t s_touch = nullptr;

static volatile uint16_t s_touch_last_raw_x = 0;
static volatile uint16_t s_touch_last_raw_y = 0;
static volatile uint16_t s_touch_last_z1 = 0;
static volatile bool s_touch_last_pressed = false;
static volatile uint32_t s_touch_read_cb_count = 0;
static volatile uint8_t s_touch_last_z1_bytes[3] = {0};
static volatile uint8_t s_touch_last_x_bytes[3] = {0};
static volatile uint8_t s_touch_last_y_bytes[3] = {0};

static void load_mirror_from_nvs() {
    nvs_handle_t h;
    if (nvs_open("display", NVS_READONLY, &h) != ESP_OK) return;

    uint8_t mx = 0, my = 0;
    if (nvs_get_u8(h, "mx", &mx) == ESP_OK) s_mirror_x = (mx != 0);
    if (nvs_get_u8(h, "my", &my) == ESP_OK) s_mirror_y = (my != 0);
    int32_t xoff = 0;
    int32_t yoff = 0;
    if (nvs_get_i32(h, "xoff", &xoff) == ESP_OK) s_lcd_x_offset = (int)xoff;
    if (nvs_get_i32(h, "yoff", &yoff) == ESP_OK) s_lcd_y_offset = (int)yoff;
    s_lcd_x_offset = std::clamp(s_lcd_x_offset, -40, 40);
    s_lcd_y_offset = std::clamp(s_lcd_y_offset, -40, 40);
    nvs_close(h);
}

static void save_mirror_to_nvs() {
    nvs_handle_t h;
    if (nvs_open("display", NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u8(h, "mx", s_mirror_x ? 1 : 0);
    (void)nvs_set_u8(h, "my", s_mirror_y ? 1 : 0);
    (void)nvs_set_i32(h, "xoff", (int32_t)s_lcd_x_offset);
    (void)nvs_set_i32(h, "yoff", (int32_t)s_lcd_y_offset);
    (void)nvs_commit(h);
    nvs_close(h);
}

static void load_touch_from_nvs() {
    nvs_handle_t h;
    if (nvs_open("touch", NVS_READONLY, &h) != ESP_OK) return;

    uint8_t mode = 0;
    uint32_t hz = 0;
    if (nvs_get_u8(h, "mode", &mode) == ESP_OK) s_touch_spi_mode = (mode <= 3) ? (int)mode : 0;
    if (nvs_get_u32(h, "hz", &hz) == ESP_OK && hz > 0) s_touch_spi_hz = (int)hz;

    uint8_t swap_xy = 0;
    if (nvs_get_u8(h, "swap", &swap_xy) == ESP_OK) s_touch_swap_xy = (swap_xy != 0);

    uint8_t mx = 0, my = 0;
    if (nvs_get_u8(h, "mx", &mx) == ESP_OK) s_touch_mirror_x = (mx != 0);
    if (nvs_get_u8(h, "my", &my) == ESP_OK) s_touch_mirror_y = (my != 0);

    uint8_t cal_en = 0;
    if (nvs_get_u8(h, "cal_en", &cal_en) == ESP_OK) s_touch_cal_enabled = (cal_en != 0);
    uint16_t v16 = 0;
    if (nvs_get_u16(h, "cal_l", &v16) == ESP_OK) s_touch_cal_left = v16;
    if (nvs_get_u16(h, "cal_r", &v16) == ESP_OK) s_touch_cal_right = v16;
    if (nvs_get_u16(h, "cal_t", &v16) == ESP_OK) s_touch_cal_top = v16;
    if (nvs_get_u16(h, "cal_b", &v16) == ESP_OK) s_touch_cal_bottom = v16;

    uint8_t aff_en = 0;
    if (nvs_get_u8(h, "aff_en", &aff_en) == ESP_OK) s_touch_affine_enabled = (aff_en != 0);
    float f = 0.0f;
    size_t sz = sizeof(float);
    if (nvs_get_blob(h, "aff_a", &f, &sz) == ESP_OK) s_touch_affine_a = f;
    sz = sizeof(float);
    if (nvs_get_blob(h, "aff_b", &f, &sz) == ESP_OK) s_touch_affine_b = f;
    sz = sizeof(float);
    if (nvs_get_blob(h, "aff_c", &f, &sz) == ESP_OK) s_touch_affine_c = f;
    sz = sizeof(float);
    if (nvs_get_blob(h, "aff_d", &f, &sz) == ESP_OK) s_touch_affine_d = f;
    sz = sizeof(float);
    if (nvs_get_blob(h, "aff_e", &f, &sz) == ESP_OK) s_touch_affine_e = f;
    sz = sizeof(float);
    if (nvs_get_blob(h, "aff_f", &f, &sz) == ESP_OK) s_touch_affine_f = f;

    uint8_t grid_en = 0;
    if (nvs_get_u8(h, "grid_en", &grid_en) == ESP_OK) s_touch_grid_enabled = (grid_en != 0);
    size_t grid_sz = sizeof(float) * s_touch_grid_dx.size();
    (void)nvs_get_blob(h, "grid_dx", s_touch_grid_dx.data(), &grid_sz);
    grid_sz = sizeof(float) * s_touch_grid_dy.size();
    (void)nvs_get_blob(h, "grid_dy", s_touch_grid_dy.data(), &grid_sz);

    int32_t pin = -1;
    if (nvs_get_i32(h, "sclk", &pin) == ESP_OK && GPIO_IS_VALID_GPIO((gpio_num_t)pin)) s_touch_sclk = (gpio_num_t)pin;
    pin = -1;
    if (nvs_get_i32(h, "mosi", &pin) == ESP_OK && GPIO_IS_VALID_GPIO((gpio_num_t)pin)) s_touch_mosi = (gpio_num_t)pin;
    pin = -1;
    if (nvs_get_i32(h, "miso", &pin) == ESP_OK && GPIO_IS_VALID_GPIO((gpio_num_t)pin)) s_touch_miso = (gpio_num_t)pin;
    nvs_close(h);
}

static void save_touch_to_nvs() {
    nvs_handle_t h;
    if (nvs_open("touch", NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u8(h, "mode", (uint8_t)s_touch_spi_mode);
    (void)nvs_set_u32(h, "hz", (uint32_t)s_touch_spi_hz);
    (void)nvs_set_u8(h, "swap", (uint8_t)(s_touch_swap_xy ? 1 : 0));
    (void)nvs_set_u8(h, "mx", (uint8_t)(s_touch_mirror_x ? 1 : 0));
    (void)nvs_set_u8(h, "my", (uint8_t)(s_touch_mirror_y ? 1 : 0));
    (void)nvs_set_u8(h, "cal_en", (uint8_t)(s_touch_cal_enabled ? 1 : 0));
    (void)nvs_set_u16(h, "cal_l", s_touch_cal_left);
    (void)nvs_set_u16(h, "cal_r", s_touch_cal_right);
    (void)nvs_set_u16(h, "cal_t", s_touch_cal_top);
    (void)nvs_set_u16(h, "cal_b", s_touch_cal_bottom);
    (void)nvs_set_u8(h, "aff_en", (uint8_t)(s_touch_affine_enabled ? 1 : 0));
    (void)nvs_set_blob(h, "aff_a", &s_touch_affine_a, sizeof(float));
    (void)nvs_set_blob(h, "aff_b", &s_touch_affine_b, sizeof(float));
    (void)nvs_set_blob(h, "aff_c", &s_touch_affine_c, sizeof(float));
    (void)nvs_set_blob(h, "aff_d", &s_touch_affine_d, sizeof(float));
    (void)nvs_set_blob(h, "aff_e", &s_touch_affine_e, sizeof(float));
    (void)nvs_set_blob(h, "aff_f", &s_touch_affine_f, sizeof(float));
    (void)nvs_set_u8(h, "grid_en", (uint8_t)(s_touch_grid_enabled ? 1 : 0));
    (void)nvs_set_blob(h, "grid_dx", s_touch_grid_dx.data(), sizeof(float) * s_touch_grid_dx.size());
    (void)nvs_set_blob(h, "grid_dy", s_touch_grid_dy.data(), sizeof(float) * s_touch_grid_dy.size());
    (void)nvs_set_i32(h, "sclk", (int32_t)s_touch_sclk);
    (void)nvs_set_i32(h, "mosi", (int32_t)s_touch_mosi);
    (void)nvs_set_i32(h, "miso", (int32_t)s_touch_miso);
    (void)nvs_commit(h);
    nvs_close(h);
}

static void apply_touch_grid(int &x, int &y) {
    if (!s_touch_grid_enabled) return;

    const float fx = (DISPLAY_WIDTH <= 1) ? 0.0f : (float)x / (float)(DISPLAY_WIDTH - 1);
    const float fy = (DISPLAY_HEIGHT <= 1) ? 0.0f : (float)y / (float)(DISPLAY_HEIGHT - 1);

    int gx = (int)std::floor(fx * 2.0f);
    int gy = (int)std::floor(fy * 2.0f);
    gx = std::clamp(gx, 0, 1);
    gy = std::clamp(gy, 0, 1);

    const float tx = std::clamp(fx * 2.0f - (float)gx, 0.0f, 1.0f);
    const float ty = std::clamp(fy * 2.0f - (float)gy, 0.0f, 1.0f);

    const auto idx = [](int ix, int iy) -> int { return iy * 3 + ix; };

    const int i00 = idx(gx + 0, gy + 0);
    const int i10 = idx(gx + 1, gy + 0);
    const int i01 = idx(gx + 0, gy + 1);
    const int i11 = idx(gx + 1, gy + 1);

    const float dx00 = s_touch_grid_dx[(size_t)i00];
    const float dx10 = s_touch_grid_dx[(size_t)i10];
    const float dx01 = s_touch_grid_dx[(size_t)i01];
    const float dx11 = s_touch_grid_dx[(size_t)i11];
    const float dy00 = s_touch_grid_dy[(size_t)i00];
    const float dy10 = s_touch_grid_dy[(size_t)i10];
    const float dy01 = s_touch_grid_dy[(size_t)i01];
    const float dy11 = s_touch_grid_dy[(size_t)i11];

    const auto lerp = [](float a, float b, float t) -> float { return a + (b - a) * t; };
    const float dx0 = lerp(dx00, dx10, tx);
    const float dx1 = lerp(dx01, dx11, tx);
    const float dy0 = lerp(dy00, dy10, tx);
    const float dy1 = lerp(dy01, dy11, tx);
    const float dx = lerp(dx0, dx1, ty);
    const float dy = lerp(dy0, dy1, ty);

    x = (int)std::lround((float)x + dx);
    y = (int)std::lround((float)y + dy);
    x = std::clamp(x, 0, DISPLAY_WIDTH - 1);
    y = std::clamp(y, 0, DISPLAY_HEIGHT - 1);
}

static void apply_touch_calibration(int &x, int &y) {
    if (!s_touch_cal_enabled) return;

    const int left = (int)s_touch_cal_left;
    const int right = (int)s_touch_cal_right;
    const int top = (int)s_touch_cal_top;
    const int bottom = (int)s_touch_cal_bottom;
    if (right - left < 20 || bottom - top < 20) return;

    x = (x - left) * (DISPLAY_WIDTH - 1) / (right - left);
    y = (y - top) * (DISPLAY_HEIGHT - 1) / (bottom - top);
    x = std::clamp(x, 0, DISPLAY_WIDTH - 1);
    y = std::clamp(y, 0, DISPLAY_HEIGHT - 1);

    if (s_touch_affine_enabled) {
        const float xf = (float)x;
        const float yf = (float)y;
        const float xa = s_touch_affine_a * xf + s_touch_affine_b * yf + s_touch_affine_c;
        const float ya = s_touch_affine_d * xf + s_touch_affine_e * yf + s_touch_affine_f;
        x = (int)std::lround(xa);
        y = (int)std::lround(ya);
        x = std::clamp(x, 0, DISPLAY_WIDTH - 1);
        y = std::clamp(y, 0, DISPLAY_HEIGHT - 1);
    }

    if (s_touch_grid_enabled) {
        apply_touch_grid(x, y);
    } else if (s_touch_quad_enabled) {
        const float yf = (float)y;
        const float yq = s_touch_quad_a * yf * yf + s_touch_quad_b * yf + s_touch_quad_c;
        y = (int)std::lround(yq);
        y = std::clamp(y, 0, DISPLAY_HEIGHT - 1);
    }
}

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
    return spi_device_transmit(s_lcd_spi, &t);
}

static esp_err_t lcd_cmd(uint8_t cmd) { return lcd_tx(&cmd, 1, false); }

static esp_err_t lcd_data(const void *data, size_t len_bytes) {
    if (!data || len_bytes == 0) return ESP_OK;
    const uint8_t *p = static_cast<const uint8_t *>(data);
    size_t remaining = len_bytes;
    while (remaining > 0) {
        const size_t chunk = std::min(remaining, LCD_SPI_CHUNK_BYTES);
        const esp_err_t err = lcd_tx(p, chunk, true);
        if (err != ESP_OK) return err;
        p += chunk;
        remaining -= chunk;
    }
    return ESP_OK;
}

static esp_err_t lcd_write_u16be(uint16_t value) {
    uint8_t b[2] = {(uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return lcd_data(b, sizeof(b));
}

static esp_err_t lcd_set_addr_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    const uint16_t x1 = x + LCD_X_OFFSET;
    const uint16_t y1 = y + LCD_Y_OFFSET;
    const uint16_t x2 = x1 + w - 1;
    const uint16_t y2 = y1 + h - 1;

    ESP_RETURN_ON_ERROR(lcd_cmd(0x2A), TAG, "CASET");
    ESP_RETURN_ON_ERROR(lcd_write_u16be(x1), TAG, "CASET x1");
    ESP_RETURN_ON_ERROR(lcd_write_u16be(x2), TAG, "CASET x2");

    ESP_RETURN_ON_ERROR(lcd_cmd(0x2B), TAG, "PASET");
    ESP_RETURN_ON_ERROR(lcd_write_u16be(y1), TAG, "PASET y1");
    ESP_RETURN_ON_ERROR(lcd_write_u16be(y2), TAG, "PASET y2");

    return lcd_cmd(0x2C); // RAMWR
}

static esp_err_t lcd_set_rotation_landscape_1(void) {
    // ST7796 MADCTL bits:
    // MY 0x80, MX 0x40, MV 0x20, ML 0x10, BGR 0x08, MH 0x04.
    //
    // For 480x320 landscape we use MV (swap axes) + MH.
    // Mirror flags are persisted in NVS and can be changed via API.
    // Most ST7796 modules are wired as BGR. If colors look "red=blue", keep BGR enabled.
    const uint8_t madctl_base = 0x2C; // MV | MH | BGR
    uint8_t madctl = madctl_base | (s_mirror_x ? 0x40 : 0) | (s_mirror_y ? 0x80 : 0);
    ESP_RETURN_ON_ERROR(lcd_cmd(0x36), TAG, "MADCTL cmd");
    return lcd_data(&madctl, 1);
}

static esp_err_t st7796_init_sequence(void) {
    ESP_LOGI(TAG, "Starting ST7796 initialization sequence...");
    
    // Init sequence adapted from LovyanGFX Panel_ST7796 defaults.
    const auto tx = [](uint8_t cmd, std::initializer_list<uint8_t> data, int delay_ms = 0) -> esp_err_t {
        ESP_LOGD(TAG, "ST7796 cmd 0x%02X, data size: %zu, delay: %d ms", cmd, data.size(), delay_ms);
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
    
    ESP_LOGI(TAG, "ST7796 initialization sequence completed successfully");
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

    if (TOUCH_IRQ != GPIO_NUM_NC && GPIO_IS_VALID_GPIO(TOUCH_IRQ)) {
        gpio_config_t icfg{};
        icfg.mode = GPIO_MODE_INPUT;
        icfg.pin_bit_mask = (1ULL << TOUCH_IRQ);
        icfg.pull_up_en = GPIO_PULLUP_ENABLE;   // XPT2046 IRQ is active-low (needs pull-up)
        icfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_config(&icfg);
    }
}

static esp_err_t display_spi_init(void) {
    ESP_LOGI(TAG, "Initializing SPI bus for display...");
    ESP_LOGI(TAG, "SPI pins: SCLK=%d, MOSI=%d, MISO=%d, CS=%d, DC=%d, RST=%d, BL=%d", 
             TFT_SCLK, TFT_MOSI, TFT_MISO, TFT_CS, TFT_DC, TFT_RST, TFT_BL);
    
    spi_bus_config_t buscfg{};
    buscfg.sclk_io_num = TFT_SCLK;
    buscfg.mosi_io_num = TFT_MOSI;
    buscfg.miso_io_num = TFT_MISO;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = (int)LCD_SPI_CHUNK_BYTES + 8;

    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi_bus_initialize");

    spi_device_interface_config_t devcfg{};
    devcfg.clock_speed_hz = TFT_SPI_CLOCK_HZ;
    devcfg.mode = 0;
    devcfg.spics_io_num = TFT_CS;
    devcfg.queue_size = TFT_SPI_QUEUE_SIZE;
    devcfg.pre_cb = lcd_spi_pre_transfer_cb;
    devcfg.flags = SPI_DEVICE_HALFDUPLEX;

    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_HOST, &devcfg, &s_lcd_spi), TAG, "spi_bus_add_device lcd");
    
    ESP_LOGI(TAG, "SPI display device initialized successfully");

    const bool touch_shares_bus = (s_touch_sclk == TFT_SCLK) && (s_touch_mosi == TFT_MOSI) && (s_touch_miso == TFT_MISO);
    s_touch_host = touch_shares_bus ? LCD_HOST : TOUCH_HOST;

    if (!touch_shares_bus) {
        ESP_LOGI(TAG, "Touch controller uses separate SPI bus");
        spi_bus_config_t tbus{};
        tbus.sclk_io_num = s_touch_sclk;
        tbus.mosi_io_num = s_touch_mosi;
        tbus.miso_io_num = s_touch_miso;
        tbus.quadwp_io_num = -1;
        tbus.quadhd_io_num = -1;
        tbus.max_transfer_sz = 64;
        ESP_RETURN_ON_ERROR(spi_bus_initialize(TOUCH_HOST, &tbus, SPI_DMA_CH_AUTO), TAG, "spi_bus_initialize touch");
    }

    return ESP_OK;
}

static void display_reset(void) {
    ESP_LOGI(TAG, "Resetting display controller...");
    gpio_set_level(TFT_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "Display reset completed");
}

static esp_err_t touch_deinit(void) {
    if (s_touch) {
        (void)esp_lcd_touch_del(s_touch);
        s_touch = nullptr;
    }
    if (s_touch_io) {
        (void)esp_lcd_panel_io_del(s_touch_io);
        s_touch_io = nullptr;
    }
    return ESP_OK;
}

static esp_err_t touch_init(void) {
    if (s_touch) return ESP_OK;

    esp_lcd_panel_io_spi_config_t io_cfg{};
    io_cfg.cs_gpio_num = (gpio_num_t)TOUCH_CS;
    io_cfg.dc_gpio_num = (gpio_num_t)GPIO_NUM_NC;
    io_cfg.spi_mode = s_touch_spi_mode;
    io_cfg.pclk_hz = (unsigned int)s_touch_spi_hz;
    io_cfg.trans_queue_depth = 3;
    io_cfg.on_color_trans_done = nullptr;
    io_cfg.user_ctx = nullptr;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.cs_ena_pretrans = 0;
    io_cfg.cs_ena_posttrans = 0;
    io_cfg.flags.dc_high_on_cmd = 0;
    io_cfg.flags.dc_low_on_data = 0;
    io_cfg.flags.dc_low_on_param = 0;
    io_cfg.flags.octal_mode = 0;
    io_cfg.flags.quad_mode = 0;
    io_cfg.flags.sio_mode = 0;
    io_cfg.flags.lsb_first = 0;
    io_cfg.flags.cs_high_active = 0;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)s_touch_host, &io_cfg, &s_touch_io), TAG, "touch io");

    // Base mapping (panel wiring): swap axes + invert Y.
    // Then apply display mirror settings so touch stays aligned with what the user sees.
    // Our display "mirror_y" flag effectively toggles the baseline Y inversion.
    esp_lcd_touch_config_t tp_cfg{};
    tp_cfg.x_max = DISPLAY_WIDTH - 1;
    tp_cfg.y_max = DISPLAY_HEIGHT - 1;
    tp_cfg.rst_gpio_num = GPIO_NUM_NC;
    tp_cfg.int_gpio_num = TOUCH_IRQ;
    tp_cfg.levels.reset = 0;
    tp_cfg.levels.interrupt = 0;
    tp_cfg.flags.swap_xy = s_touch_swap_xy ? 1 : 0;
    tp_cfg.flags.mirror_x = s_touch_mirror_x ? 1 : 0;
    tp_cfg.flags.mirror_y = s_touch_mirror_y ? 1 : 0;
    tp_cfg.process_coordinates = nullptr;
    tp_cfg.interrupt_callback = nullptr;
    tp_cfg.user_data = nullptr;
    tp_cfg.driver_data = nullptr;

    const esp_err_t err = esp_lcd_touch_new_spi_xpt2046(s_touch_io, &tp_cfg, &s_touch);
    if (err != ESP_OK) {
        (void)touch_deinit();
        return err;
    }

    ESP_LOGI(TAG, "Touch initialized (XPT2046, mode=%d, hz=%d, host=%d)", s_touch_spi_mode, s_touch_spi_hz, (int)s_touch_host);
    return ESP_OK;
}

static bool touch_read_once(int &x, int &y, uint16_t &z) {
    if (!s_touch) return false;
    (void)esp_lcd_touch_read_data(s_touch);
    esp_lcd_touch_point_data_t points[1]{};
    uint8_t point_cnt = 0;
    (void)esp_lcd_touch_get_data(s_touch, points, &point_cnt, 1);
    if (point_cnt == 0) {
        x = 0;
        y = 0;
        z = 0;
        return false;
    }
    x = (int)points[0].x;
    y = (int)points[0].y;
    z = points[0].strength;
    return true;
}

static int median3(int a, int b, int c) {
    if (a > b) std::swap(a, b);
    if (b > c) std::swap(b, c);
    if (a > b) std::swap(a, b);
    return b;
}

void display_driver_init(void) {
    load_mirror_from_nvs();

    s_touch_swap_xy = true;
    s_touch_mirror_x = s_mirror_x;
    s_touch_mirror_y = s_mirror_y ? false : true;

    load_touch_from_nvs();
    display_gpio_init();
    
    esp_err_t err = display_spi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(err));
        return;
    }
    
    display_reset();
    
    err = st7796_init_sequence();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ST7796 controller: %s", esp_err_to_name(err));
        gpio_set_level(TFT_BL, 0);
        return;
    }
    
    err = touch_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize touch controller: %s", esp_err_to_name(err));
    }

    gpio_set_level(TFT_BL, 1);
    ESP_LOGI(TAG, "Display initialized successfully");
}

bool display_driver_blit_rgb565(int x, int y, int w, int h, const uint16_t *data) {
    if (!data || w <= 0 || h <= 0) return false;

    int x1 = x + s_lcd_x_offset;
    int y1 = y + s_lcd_y_offset;
    int x2 = x1 + w - 1;
    int y2 = y1 + h - 1;

    int crop_left = 0;
    int crop_top = 0;
    int crop_right = 0;
    int crop_bottom = 0;

    if (x1 < 0) {
        crop_left = -x1;
        x1 = 0;
    }
    if (y1 < 0) {
        crop_top = -y1;
        y1 = 0;
    }
    if (x2 >= DISPLAY_WIDTH) crop_right = x2 - (DISPLAY_WIDTH - 1);
    if (y2 >= DISPLAY_HEIGHT) crop_bottom = y2 - (DISPLAY_HEIGHT - 1);

    const int out_w = w - crop_left - crop_right;
    const int out_h = h - crop_top - crop_bottom;
    if (out_w <= 0 || out_h <= 0) return false;

    if (lcd_set_addr_window((uint16_t)x1, (uint16_t)y1, (uint16_t)out_w, (uint16_t)out_h) != ESP_OK) {
        return false;
    }

    if (!s_lcd_swap_bytes) {
        if (crop_left == 0 && crop_right == 0) {
            const uint8_t *buf = (const uint8_t *)(data + (crop_top * w));
            const size_t bytes = (size_t)w * (size_t)out_h * 2;
            (void)lcd_data(buf, bytes);
        } else {
            for (int row = 0; row < out_h; ++row) {
                const int src_row = crop_top + row;
                const uint8_t *buf = (const uint8_t *)(data + (src_row * w + crop_left));
                (void)lcd_data(buf, (size_t)out_w * 2);
            }
        }
        return true;
    }

    // Byte-swap RGB565 pixels into a scratch row buffer before writing.
    static std::vector<uint16_t> rowbuf;
    if ((int)rowbuf.size() < out_w) rowbuf.resize((size_t)out_w);

    for (int row = 0; row < out_h; ++row) {
        const int src_row = crop_top + row;
        const uint16_t *src = data + (src_row * w + crop_left);
        for (int col = 0; col < out_w; ++col) {
            const uint16_t px = src[col];
            rowbuf[(size_t)col] = (uint16_t)((px >> 8) | (px << 8));
        }
        (void)lcd_data((const uint8_t *)rowbuf.data(), (size_t)out_w * 2);
    }

    return true;
}

bool display_driver_touch_probe(uint16_t *raw_x, uint16_t *raw_y, uint16_t *z1) {
    if (!s_touch) {
        if (raw_x) *raw_x = 0;
        if (raw_y) *raw_y = 0;
        if (z1) *z1 = 0;
        return false;
    }

    const bool pressed = true;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    uint16_t z0 = 0, z1v = 0, z2 = 0;
    if (!touch_read_once(x0, y0, z0)) {
        if (raw_x) *raw_x = 0;
        if (raw_y) *raw_y = 0;
        if (z1) *z1 = 0;
        s_touch_last_pressed = false;
        return false;
    }
    // Take a couple of extra samples to reduce noise.
    if (!touch_read_once(x1, y1, z1v)) {
        x1 = x0;
        y1 = y0;
        z1v = z0;
    }
    if (!touch_read_once(x2, y2, z2)) {
        x2 = x0;
        y2 = y0;
        z2 = z0;
    }

    int lx = median3(x0, x1, x2);
    int ly = median3(y0, y1, y2);
    uint16_t lz = (uint16_t)median3((int)z0, (int)z1v, (int)z2);

    // Depending on XPT2046 config, driver may return raw ADC (0..4095) or already-scaled coords.
    // Clamp broadly, then apply our calibration into display coordinates.
    static constexpr int RAW_MAX = 4095;
    lx = std::clamp(lx, 0, RAW_MAX);
    ly = std::clamp(ly, 0, RAW_MAX);
    apply_touch_calibration(lx, ly);

    s_touch_last_pressed = pressed;
    s_touch_last_raw_x = (uint16_t)lx;
    s_touch_last_raw_y = (uint16_t)ly;
    s_touch_last_z1 = lz;

    // Raw SPI bytes are not exposed by esp_lcd_touch; keep legacy fields zeroed.
    s_touch_last_z1_bytes[0] = 0;
    s_touch_last_z1_bytes[1] = 0;
    s_touch_last_z1_bytes[2] = 0;
    s_touch_last_x_bytes[0] = 0;
    s_touch_last_x_bytes[1] = 0;
    s_touch_last_x_bytes[2] = 0;
    s_touch_last_y_bytes[0] = 0;
    s_touch_last_y_bytes[1] = 0;
    s_touch_last_y_bytes[2] = 0;

    if (raw_x) *raw_x = (uint16_t)lx;
    if (raw_y) *raw_y = (uint16_t)ly;
    if (z1) *z1 = lz;
    return true;
}

bool display_driver_touch_probe_raw(uint16_t *raw_x, uint16_t *raw_y, uint16_t *z1) {
    if (!s_touch) {
        if (raw_x) *raw_x = 0;
        if (raw_y) *raw_y = 0;
        if (z1) *z1 = 0;
        return false;
    }

    (void)esp_lcd_touch_read_data(s_touch);

    esp_lcd_touch_point_data_t points[1]{};
    uint8_t point_cnt = 0;
    (void)esp_lcd_touch_get_data(s_touch, points, &point_cnt, 1);
    const bool pressed = point_cnt > 0;

    int lx = pressed ? (int)points[0].x : 0;
    int ly = pressed ? (int)points[0].y : 0;
    const uint16_t lz = pressed ? points[0].strength : 0;

    if (pressed) {
        static constexpr int RAW_MAX = 4095;
        lx = std::clamp(lx, 0, RAW_MAX);
        ly = std::clamp(ly, 0, RAW_MAX);
    }

    s_touch_last_pressed = pressed;
    s_touch_last_raw_x = (uint16_t)lx;
    s_touch_last_raw_y = (uint16_t)ly;
    s_touch_last_z1 = lz;

    if (raw_x) *raw_x = (uint16_t)lx;
    if (raw_y) *raw_y = (uint16_t)ly;
    if (z1) *z1 = lz;
    return pressed;
}

void display_driver_get_touch_debug(uint16_t *raw_x, uint16_t *raw_y, uint16_t *z1, bool *pressed) {
    if (raw_x) *raw_x = s_touch_last_raw_x;
    if (raw_y) *raw_y = s_touch_last_raw_y;
    if (z1) *z1 = s_touch_last_z1;
    if (pressed) *pressed = s_touch_last_pressed;
}

void display_driver_get_touch_stats(uint32_t *read_cb_count) {
    if (read_cb_count) *read_cb_count = s_touch_read_cb_count;
}

void display_driver_get_touch_last_bytes(uint8_t out_z1[3], uint8_t out_x[3], uint8_t out_y[3]) {
    if (out_z1) {
        out_z1[0] = s_touch_last_z1_bytes[0];
        out_z1[1] = s_touch_last_z1_bytes[1];
        out_z1[2] = s_touch_last_z1_bytes[2];
    }
    if (out_x) {
        out_x[0] = s_touch_last_x_bytes[0];
        out_x[1] = s_touch_last_x_bytes[1];
        out_x[2] = s_touch_last_x_bytes[2];
    }
    if (out_y) {
        out_y[0] = s_touch_last_y_bytes[0];
        out_y[1] = s_touch_last_y_bytes[1];
        out_y[2] = s_touch_last_y_bytes[2];
    }
}

void display_driver_get_touch_spi(int *mode, int *clock_hz) {
    if (mode) *mode = s_touch_spi_mode;
    if (clock_hz) *clock_hz = s_touch_spi_hz;
}

void display_driver_get_touch_transform(bool *swap_xy, bool *mirror_x, bool *mirror_y) {
    if (swap_xy) *swap_xy = s_touch_swap_xy;
    if (mirror_x) *mirror_x = s_touch_mirror_x;
    if (mirror_y) *mirror_y = s_touch_mirror_y;
}

bool display_driver_set_touch_transform(bool swap_xy, bool mirror_x, bool mirror_y) {
    s_touch_swap_xy = swap_xy;
    s_touch_mirror_x = mirror_x;
    s_touch_mirror_y = mirror_y;
    save_touch_to_nvs();

    if (!s_touch_io && !s_touch) return true;
    if (touch_deinit() != ESP_OK) return false;
    return touch_init() == ESP_OK;
}

void display_driver_get_touch_calibration(bool *enabled, uint16_t *left, uint16_t *right, uint16_t *top, uint16_t *bottom) {
    if (enabled) *enabled = s_touch_cal_enabled;
    if (left) *left = s_touch_cal_left;
    if (right) *right = s_touch_cal_right;
    if (top) *top = s_touch_cal_top;
    if (bottom) *bottom = s_touch_cal_bottom;
}

bool display_driver_set_touch_calibration(bool enabled, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom) {
    if (right <= left + 10 || bottom <= top + 10) return false;
    // Allow calibration bounds larger than screen size to adjust scale.
    // XPT2046 raw coordinates are 12-bit (0..4095).
    static constexpr uint16_t RAW_MAX = 4095;
    if (left > RAW_MAX || right > RAW_MAX || top > RAW_MAX || bottom > RAW_MAX) return false;
    s_touch_cal_enabled = enabled;
    s_touch_cal_left = left;
    s_touch_cal_right = right;
    s_touch_cal_top = top;
    s_touch_cal_bottom = bottom;
    save_touch_to_nvs();
    return true;
}

void display_driver_reset_touch_calibration(void) {
    s_touch_cal_enabled = false;
    s_touch_cal_left = 0;
    s_touch_cal_right = DISPLAY_WIDTH - 1;
    s_touch_cal_top = 0;
    s_touch_cal_bottom = DISPLAY_HEIGHT - 1;
    save_touch_to_nvs();
}

bool display_driver_is_touch_calibrated(void) {
    return s_touch_cal_enabled || s_touch_affine_enabled || s_touch_grid_enabled;
}

void display_driver_get_touch_affine(bool *enabled, float *a, float *b, float *c, float *d, float *e, float *f) {
    if (enabled) *enabled = s_touch_affine_enabled;
    if (a) *a = s_touch_affine_a;
    if (b) *b = s_touch_affine_b;
    if (c) *c = s_touch_affine_c;
    if (d) *d = s_touch_affine_d;
    if (e) *e = s_touch_affine_e;
    if (f) *f = s_touch_affine_f;
}

bool display_driver_set_touch_affine(bool enabled, float a, float b, float c, float d, float e, float f) {
    s_touch_affine_enabled = enabled;
    s_touch_affine_a = a;
    s_touch_affine_b = b;
    s_touch_affine_c = c;
    s_touch_affine_d = d;
    s_touch_affine_e = e;
    s_touch_affine_f = f;
    save_touch_to_nvs();
    return true;
}

void display_driver_get_touch_grid(bool *enabled, float dx_out[9], float dy_out[9]) {
    if (enabled) *enabled = s_touch_grid_enabled;
    if (dx_out) std::copy(s_touch_grid_dx.begin(), s_touch_grid_dx.end(), dx_out);
    if (dy_out) std::copy(s_touch_grid_dy.begin(), s_touch_grid_dy.end(), dy_out);
}

bool display_driver_set_touch_grid(bool enabled, const float dx[9], const float dy[9]) {
    s_touch_grid_enabled = enabled;
    if (enabled) {
        if (!dx || !dy) return false;
        std::copy(dx, dx + s_touch_grid_dx.size(), s_touch_grid_dx.begin());
        std::copy(dy, dy + s_touch_grid_dy.size(), s_touch_grid_dy.begin());
    } else {
        s_touch_grid_dx.fill(0.0f);
        s_touch_grid_dy.fill(0.0f);
    }
    // Grid and quadratic corrections should not stack.
    if (s_touch_grid_enabled) s_touch_quad_enabled = false;
    save_touch_to_nvs();
    return true;
}

void display_driver_get_touch_pins(int *sclk, int *mosi, int *miso) {
    if (sclk) *sclk = (int)s_touch_sclk;
    if (mosi) *mosi = (int)s_touch_mosi;
    if (miso) *miso = (int)s_touch_miso;
}

bool display_driver_set_touch_pins(int sclk, int mosi, int miso) {
    if (!GPIO_IS_VALID_GPIO((gpio_num_t)sclk)) return false;
    if (!GPIO_IS_VALID_GPIO((gpio_num_t)mosi)) return false;
    if (!GPIO_IS_VALID_GPIO((gpio_num_t)miso)) return false;

    s_touch_sclk = (gpio_num_t)sclk;
    s_touch_mosi = (gpio_num_t)mosi;
    s_touch_miso = (gpio_num_t)miso;
    save_touch_to_nvs();

    // Caller should reboot to re-init SPI buses with the new pin mapping.
    return true;
}

bool display_driver_set_touch_spi(int mode, int clock_hz) {
    if (mode < 0 || mode > 3) return false;
    if (clock_hz < 50 * 1000 || clock_hz > 2 * 1000 * 1000) return false;

    s_touch_spi_mode = mode;
    s_touch_spi_hz = clock_hz;
    save_touch_to_nvs();

    if (!s_touch_io && !s_touch) return true;

    if (touch_deinit() != ESP_OK) return false;
    return touch_init() == ESP_OK;
}

void display_driver_get_mirror(bool *mirror_x, bool *mirror_y) {
    if (mirror_x) *mirror_x = s_mirror_x;
    if (mirror_y) *mirror_y = s_mirror_y;
}

void display_driver_set_mirror(bool mirror_x, bool mirror_y) {
    s_mirror_x = mirror_x;
    s_mirror_y = mirror_y;
    save_mirror_to_nvs();

    if (s_lcd_spi) {
        (void)lcd_set_rotation_landscape_1();
    }

    if (s_touch) {
        // mirror_y toggles baseline Y inversion (see touch_init).
        (void)esp_lcd_touch_set_mirror_x(s_touch, s_mirror_x);
        (void)esp_lcd_touch_set_mirror_y(s_touch, !s_mirror_y);
    }
}

void display_driver_get_offset(int *x_offset, int *y_offset) {
    if (x_offset) *x_offset = s_lcd_x_offset;
    if (y_offset) *y_offset = s_lcd_y_offset;
}

bool display_driver_set_offset(int x_offset, int y_offset) {
    if (x_offset < -40 || x_offset > 40) return false;
    if (y_offset < -40 || y_offset > 40) return false;
    s_lcd_x_offset = x_offset;
    s_lcd_y_offset = y_offset;
    save_mirror_to_nvs();
    return true;
}
