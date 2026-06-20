[OPEN] kiln-calibration-wdt

## Симптоми
- При натисканні кнопки `Калібрування` в `Settings` UI зависає.
- Через ~30 с спрацьовує `task_wdt` для task `ui`.
- У backtrace видно шлях через `lv_buttonmatrix_event`, `keyboard_open_for_label`, `programs_update_info_from_model`, а також `create_screen__1` / `ui_programs_persist_load`.

## Очікувана поведінка
- Вхід у секцію `Калібрування` має відкриватися без зависання, без WDT і без запуску сторонньої логіки `Programs/keyboard`.

## Гіпотези
- H1: Під час переходу в `Калібрування` випадково стріляє залишковий `buttonmatrix/keyboard` event з іншого екрану, який запускає `keyboard_open_for_label`.
- H2: `programs_update_info_from_model()` все ще виконується поза `SCREEN_ID_PROGRAMS`, і саме це блокує UI-path під час входу в `Settings -> Calibration`.
- H3: `ui_settings_build_kiln_calibration_async()` створює/оновлює об'єкти так, що побічно запускається LVGL event chain на старих об'єктах.
- H4: Backtrace частково змішаний через старі/відкладені LVGL callbacks, а справжній root cause сидить у build/layout path секції `Калібрування`.

## План
- Додати інструментацію в `menu_item_cb`, `ui_settings_build_kiln_calibration_async`, `keyboard_open_for_label`, `programs_update_info_from_model`.
- Зібрати runtime-докази для того, який саме event і на якому screen id запускає небажаний path.
- Після підтвердження root cause внести мінімальний fix і перевірити `post-fix`.
