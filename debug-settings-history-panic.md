# Debug Session: settings-history-panic [OPEN]

## Симптоми
- На `Логи помилок` у картці `Помилка` в полі стану показується `нема помилки`, хоча термопара фізично не підключена і в логах є `fault=1 detail=0x01 open=1`.
- На головному екрані та в `Логи помилок` стан помилки відображається неузгоджено.
- При відкритті `Історія` після `history_rebuild_list` виникає `StoreProhibited`.
- Backtrace знову проходить через `keyboard_close -> programs_update_info_from_model -> create_screen__1`.

## Очікування
- `Логи помилок` показує актуальний fault/status так само, як його бачить термальний контролер.
- `Історія` відкривається без panic/WDT.

## Гіпотези
1. `Логи помилок` читає latched fault state, а не поточний thermocouple status, тому `open=1` не потрапляє в UI статус.
2. `keyboard_close()` під час навігації в `Історія` все ще тригерить програмний UI update (`programs_update_info_from_model`) через живі pointers/flags.
3. У `create_screen__1` або пов'язаному `post_scale` лишився live `textarea/label/buttonmatrix` state, який торкається redraw під час видалення overlay.
4. `history_rebuild_list` залишає один із дочірніх LVGL об'єктів у напіввалідному стані, а наступний redraw падає вже під час стороннього UI update.
5. `Логи помилок` і головний екран використовують різні джерела істини для sensor fault, тому одне місце бачить `open`, а інше ні.

## План
1. Зібрати точні точки спостереження для error-state UI і шляху `keyboard_close -> programs_update_info_from_model`.
2. Додати тільки instrumentation у `screens.c`/`actions.c`.
3. Перевірити логи до і після відкриття `Історія`.
4. Лише після доказів вносити мінімальний логічний фікс.

## Докази (addr2line)
- Panic під час відкриття `Історія` відбувається у процесі побудови списку після `history_rebuild_list: after_clean ...`.
- Розшифровка адрес (через `xtensa-esp32s3-elf-addr2line` + актуальний `build_verify/trae_controller.elf`) показує:
  - `lv_obj_class_create_obj -> lv_button_create -> ui_settings_history_rebuild_list_ui (screens.c:3427)`
- Симптом `StoreProhibited` з `EXCVADDR=0x00000050` узгоджується з записом у NULL після невдалої алокації (OOM) всередині створення LVGL-об'єкта.

## Мінімальний фікс (після доказів)
- Зменшити кількість LVGL-об'єктів у списку `Історія` і прибрати `lv_btn_create` для кожного рядка.
- Кожен рядок: `lv_obj_create + LV_OBJ_FLAG_CLICKABLE + 1 label (2 рядки тексту)`.
