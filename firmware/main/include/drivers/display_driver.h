#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void display_driver_init(void);

bool display_driver_blit_rgb565(int x, int y, int w, int h, const uint16_t *data);

// Display orientation helpers (persisted in NVS).
void display_driver_get_mirror(bool *mirror_x, bool *mirror_y);
void display_driver_set_mirror(bool mirror_x, bool mirror_y);
void display_driver_get_offset(int *x_offset, int *y_offset);
bool display_driver_set_offset(int x_offset, int y_offset);

// Touch debug helpers.
void display_driver_get_touch_debug(uint16_t *raw_x, uint16_t *raw_y, uint16_t *z1, bool *pressed);
void display_driver_get_touch_spi(int *mode, int *clock_hz);
bool display_driver_set_touch_spi(int mode, int clock_hz);
void display_driver_get_touch_transform(bool *swap_xy, bool *mirror_x, bool *mirror_y);
bool display_driver_set_touch_transform(bool swap_xy, bool mirror_x, bool mirror_y);
void display_driver_get_touch_calibration(bool *enabled, uint16_t *left, uint16_t *right, uint16_t *top, uint16_t *bottom);
bool display_driver_set_touch_calibration(bool enabled, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom);
void display_driver_reset_touch_calibration(void);
bool display_driver_touch_probe(uint16_t *raw_x, uint16_t *raw_y, uint16_t *z1);
bool display_driver_touch_probe_raw(uint16_t *raw_x, uint16_t *raw_y, uint16_t *z1);
void display_driver_get_touch_stats(uint32_t *read_cb_count);
void display_driver_get_touch_last_bytes(uint8_t out_z1[3], uint8_t out_x[3], uint8_t out_y[3]);

// Touch pin helpers (persisted in NVS). NOTE: changing pins requires reboot to take effect.
void display_driver_get_touch_pins(int *sclk, int *mosi, int *miso);
bool display_driver_set_touch_pins(int sclk, int mosi, int miso);

#ifdef __cplusplus
}
#endif
