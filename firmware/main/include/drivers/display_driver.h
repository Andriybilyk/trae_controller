#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void display_driver_init(void);

bool display_driver_blit_rgb565(int x, int y, int w, int h, const uint16_t *data);
bool display_driver_wait_vsync(uint32_t timeout_ms);
bool display_driver_p4_begin_frame(uint16_t **out_fb, int *out_w, int *out_h);
bool display_driver_p4_present_frame(void);
bool display_driver_p4_present_frame_region(int x, int y, int w, int h);
void display_driver_p4_cancel_frame(void);

// Backlight control (0..100%).
uint8_t display_driver_get_backlight_percent(void);
void display_driver_set_backlight_percent(uint8_t percent);

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
bool display_driver_is_touch_calibrated(void);
void display_driver_get_touch_affine(bool *enabled, float *a, float *b, float *c, float *d, float *e, float *f);
bool display_driver_set_touch_affine(bool enabled, float a, float b, float c, float d, float e, float f);
void display_driver_get_touch_grid(bool *enabled, float dx_out[9], float dy_out[9]);
bool display_driver_set_touch_grid(bool enabled, const float dx[9], const float dy[9]);
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
