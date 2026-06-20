# План міграції контролера на ESP-IDF 5.4.2 та LVGL

Цей документ містить інструкції для переведення проекту контролера на стабільну базу ESP-IDF 5.4.2 з використанням графічного двигуна LVGL та архітектури, перевіреної в проекті `esp_brookesia_phone`.

## 🛠 Технічний стек
- **Framework:** ESP-IDF v5.4.2 (стабільна версія для ESP32-P4)
- **Graphics:** LVGL v8.3.x або v9.x (рекомендовано v8.3 для Brookesia)
- **Display Driver:** ST7701 (MIPI DSI, 2-lane, 480x800)
- **Touch Driver:** GT911 (I2C)
- **Hardware:** ESP32-P4 (Core v1.3 p4)

## 📍 Ключові налаштування (sdkconfig)

Для досягнення плавності без мигання необхідно встановити наступні параметри:

### 1. Пам'ять (PSRAM)
Пропускна здатність PSRAM критична для роздільної здатності 480x800.
```ini
CONFIG_SPIRAM=y
CONFIG_SPIRAM_SPEED_200M=y
CONFIG_SPIRAM_X16_MODE=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
```

### 2. Дисплей та LVGL (Анти-флікер)
Використання подвійної буферизації в PSRAM та MIPI DSI V-Sync.
```ini
CONFIG_BSP_LCD_DPI_BUFFER_NUMS=2
CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR=y
CONFIG_BSP_DISPLAY_LVGL_DIRECT_MODE=y
CONFIG_LV_MEM_CUSTOM=y
CONFIG_LV_MEM_CUSTOM_INCLUDE="esp_heap_caps.h"
CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y
```

### 3. FreeRTOS (Стабільність)
Дозвіл на розміщення завдань у PSRAM для складних UI-додатків.
```ini
CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y
CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
```

## 🚀 Послідовність ініціалізації (main.cpp)

Для стабільного старту рекомендується наступний порядок у `app_main`:

1. **NVS & Storage:** Ініціалізація NVS та монтування SPIFFS (для іконок/шрифтів).
2. **BSP Display:**
   - Налаштування `bsp_display_cfg_t` з `buff_spiram = true`.
   - Запуск `bsp_display_start_with_config()`.
   - Увімкнення підсвічування після ініціалізації LVGL.
3. **Touch:** Ініціалізація GT911. Якщо тач не відповідає, створення dummy-пристрою для запобігання крашу системи Brookesia.
4. **LVGL Lock:** Завжди використовувати `bsp_display_lock()` при маніпуляціях з об'єктами LVGL.

## 🔄 Альбомна орієнтація (Landscape 800x480)

Для вашого контролера печі, щоб отримати стабільне зображення 800x480 в альбомному режимі:

### 1. Налаштування драйвера дисплея
В коді ініціалізації BSP (`esp32_p4_function_ev_board.c` або ваш `main.cpp`) необхідно налаштувати `swap_xy`. Оскільки фізична матриця зазвичай має орієнтацію 480x800, ми міняємо осі на рівні драйвера:

```cpp
const lvgl_port_display_cfg_t disp_cfg = {
    .hres = 800, // Вказуємо ширину як 800
    .vres = 480, // Вказуємо висоту як 480
    .rotation = {
        .swap_xy = true,  // Міняємо X та Y місцями
        .mirror_x = false,
        .mirror_y = false,
    },
    // ... інші налаштування з прикладу Brookesia
};
```

### 2. Координати тачскріна
Для тачскріна GT911 також потрібно увімкнути `swap_xy` в конфігурації `esp_lcd_touch_config_t`, щоб натискання збігалися з альбомним інтерфейсом:
```cpp
.flags = {
    .swap_xy = 1,
    .mirror_x = 0,
    .mirror_y = 0,
}
```

### 3. Інтеграція екранів Pico Pixel
Коли ви отримаєте файли з Pico Pixel:
1. Додайте згенеровані файли до папки `main/ui/`.
2. В `main.cpp` після ініціалізації дисплея додайте:
```cpp
bsp_display_lock(0);
ui_init(); // Функція ініціалізації з Pico Pixel
bsp_display_unlock();
```

## 🎨 Особливості для контролера печі
- Використовуйте шрифти великого розміру для читабельності температури (монтуються через SPIFFS).
- Всі графічні елементи в Pico Pixel мають бути створені під розмір **800x480**.

## 📦 Залежності (idf_component.yml)
Обов'язкові компоненти для вашого контролера:
- `espressif/esp_lvgl_port`
- `espressif/esp_lcd_st7701`
- `espressif/esp_lcd_touch_gt911`
- `espressif/esp32_p4_function_ev_board`

---
*Примітка: Всі налаштування базуються на успішному запуску прошивки Brookesia Phone на вашому залізі.*
