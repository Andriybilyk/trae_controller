#pragma once

#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796     _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Light_PWM     _light_instance;
  lgfx::Touch_XPT2046 _touch_instance;

public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;     // FSPI=SPI2_HOST, HSPI=SPI3_HOST
      cfg.spi_mode = 0;
      cfg.freq_write = 20000000;    // Lower to 20MHz for stability
      cfg.freq_read  = 16000000;
      cfg.spi_3wire  = true;        // Try 3-wire SPI (often needed for ST7796S without MISO)
      cfg.use_lock   = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 12;
      cfg.pin_mosi = 11;
      cfg.pin_miso = 13;
      cfg.pin_dc   = 10;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs           =    9;
      cfg.pin_rst          =    3;  // Connected to GPIO 3
      cfg.pin_busy         =   -1;
      cfg.panel_width      =   320;
      cfg.panel_height     =   480;
      cfg.offset_x         =     0;
      cfg.offset_y         =     0;
      cfg.offset_rotation  =     0;
      cfg.dummy_read_pixel =     8;
      cfg.dummy_read_bits  =     1;
      cfg.readable         =  true;
      cfg.invert           = false;  // Set back to false if colors are inverted
      cfg.rgb_order        = false;  // Color Match Check: Try false (RGB) first, if red is blue, set to true (BGR)
      cfg.dlen_16bit       = false;
      cfg.bus_shared       =  true;

      // Force software reset via delay if needed, but not setRstValue (it's internal)
      // _panel_instance.setRstValue(0); 

      _panel_instance.config(cfg);
    }

    // Backlight configuration
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = 18;     // Connected to GPIO 18
      cfg.invert = false;
      cfg.freq   = 12000;
      cfg.pwm_channel = 7;

      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }

    // Touch configuration
    {
      auto cfg = _touch_instance.config();
      cfg.x_min      = 0;    // Allow full raw range
      cfg.x_max      = 4095;
      cfg.y_min      = 0;
      cfg.y_max      = 4095;
      cfg.pin_int    = -1;
      cfg.bus_shared = true;
      cfg.offset_rotation = 0;
      cfg.spi_host = SPI2_HOST;
      cfg.freq = 1000000; // 1MHz for touch is usually safe
      cfg.pin_sclk = 12;
      cfg.pin_mosi = 11;
      cfg.pin_miso = 13;
      cfg.pin_cs   = 4; // Using GPIO 4 for Touch CS as per config.h
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }

    setPanel(&_panel_instance);
  }
};

extern LGFX tft;
