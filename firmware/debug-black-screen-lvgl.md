[OPEN] Debug session: black-screen-lvgl

## Симптом
- Після прошивки: чорний екран (очікується запуск LVGL UI)

## Гіпотези (фальсифіковані)
1) Дисплей ініціалізується, але підсвітка (backlight) не вмикається → чорний екран, у логах дисплея без помилок.
2) LVGL UI не стартує (не викликається `lvgl_ui_run()` або таск не створюється) → у логах немає “LVGL UI starting…”.
3) Є креш/ресет під час ініціалізації дисплея/тачу/LVGL → у логах Guru Meditation/abort або циклічні рестарти.
4) LVGL стартує, але екран не завантажується/об’єкти не створюються → у логах є LVGL старт, але немає переходу на screen_main.
5) Невірна конфігурація панелі (RGB timing/pins) → ініціалізація проходить частково/з попередженнями, але фізично кадр не відображається.

## Докази
- PRE: `UI backend selected: Slint` + `brightness=100 backlight=off` + креш (Instruction access fault) до першого кадру.
- POST: `UI backend selected: LVGL` + `DISPLAY: Display initialized successfully` + `LVGL UI initialized, entering main loop` (без креша).
- POST2: `BACKLIGHT first_frame_ready percent=100` + `P4 BL apply=100 ... lock=1` → підсвітка має вмикатись після першого flush LVGL.

## Наступні кроки
1) Перевірити фізично: чи з’явився UI (не чорний екран).
2) Якщо все ще чорний: додам точкові логи для backlight та перший flush LVGL.
