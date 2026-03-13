# ESP-IDF Firmware (No Arduino/LovyanGFX/ArduinoJson)

This `firmware/` folder is a standard ESP-IDF project (`idf.py` + CMake).

## Prereqs

- ESP-IDF 5.x installed and activated (`IDF_PATH` set)
- ESP32-S3 toolchain working

## Build

From `firmware/`:

```powershell
idf.py set-target esp32s3
idf.py build
```

Dependencies are declared in `firmware/main/idf_component.yml` and will be fetched by the ESP-IDF Component Manager.

## Flash + Monitor

```powershell
idf.py -p COM5 flash monitor
```

## Notes

- Display is driven via native ESP-IDF SPI in `firmware/main/display_driver.cpp` (ST7796S + XPT2046).
- Touch mapping may need tweaks depending on the panel wiring (see `display_driver_lvgl_touch_read`).

