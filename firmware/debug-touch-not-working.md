[OPEN] Debug session: touch-not-working

## Симптом
- Тач не працює (немає реакції на натискання/свайп у UI).

## Очікувана поведінка
- Натискання та свайпи мають реєструватись у LVGL (pointer indev), з коректними координатами.

## Гіпотези (фальсифіковані)
1) GT911 не віддає точки (point_cnt=0) або read_data() повертає помилку → проблема I2C/ініціалізації.
2) LVGL indev read_cb не викликається або викликається, але завжди повертає RELEASED → проблема в інтеграції LVGL ↔ touch_probe.
3) Координати тачу приходять, але мапінг/клампінг робить їх некоректними → LVGL ігнорує взаємодію.
4) UI loop завис/перевантажений → інпут не обробляється.

## План збору доказів
- Додати точкову інструментацію: point_cnt/x/y/err у GT911 readout + state/coords у LVGL read_cb.
- Перепрошити, відтворити натискання, зібрати pre-run логи.

## Поточні спостереження
- У `main/ui/lvgl_backend.cpp` вже є `DBG_INDEV init/loop` і `DBG_LVGL_TOUCH`.
- У `main/drivers/display_driver.cpp` вже є `DBG_TOUCH xpt2046` і `DBG_TOUCH_PROBE`.
- За попередніми логами `lv_indev` існує, його timer `paused=0`, але при натисканнях не видно `DBG_LVGL_TOUCH` і не видно `DBG_TOUCH_PROBE`.
- Безпосередньо перед цією сесією у UI loop повернуто ручний `lv_tick_inc(dt_ms)`; це ще треба перевірити в runtime після прошивки.

## Наступний крок
- Зібрати свіжий pre-fix runtime після повернення `lv_tick_inc(dt_ms)`.
- Якщо `DBG_LVGL_TOUCH` з'явиться, проблема була в tick path / timer servicing.
- Якщо `DBG_INDEV` є, а `DBG_LVGL_TOUCH` далі немає, копати в LVGL indev scheduling / реєстрацію callback.
- Якщо `DBG_LVGL_TOUCH` є, але `DBG_TOUCH_PROBE` немає, копати в сам `display_driver_touch_probe()` або збірку/прошивку не з тим кодом.
