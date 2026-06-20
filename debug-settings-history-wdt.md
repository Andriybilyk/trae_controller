# [OPEN] settings-history-wdt

## Симптом
- Після натискання на `Налаштування -> Історія` UI зависає.
- Спрацьовує `task_wdt` для таска `ui`.
- У стеку є `lv_obj_clean`, `pwd_char_hider`, `create_screen__1`, `programs_update_info_from_model`, `confirm_prog_cancel_cb`.

## Очікувана поведінка
- Секція `Історія` відкривається без зависання.
- Список історії і графік не викликають WDT і не чіпають сторонні екрани.

## Гіпотези
1. Під час відкриття `Історія` десь викликається очищення/перебудова об'єкта з textarea або keyboard overlay, і `lv_obj_clean()` на великому дереві блокує UI.
2. Перехід у `Історія` опосередковано тригерить старий шлях `confirm_prog_cancel_cb -> programs_update_info_from_model`, тобто знову є cross-screen побічний ефект із Programs/Edit.
3. Lazy-load історії будує занадто важке дерево LVGL за один прохід (`lv_obj_clean` + багато children), тому UI не встигає скидати watchdog.
4. Подія меню для `idx=4` відкриває секцію коректно, але паралельно лишається активний transient/textarea timer (`pwd_char_hider`), який б'є в невалідний або великий tree.
5. Проблема не в самій історії, а в побічному переході на `SCREEN_ID__1` або пов'язаному recreate екрана, що видно зі стеку `create_screen__1`.

## План
1. Додати точкову інструментацію навколо `menu_item_cb`, `ui_settings_set_section`, lazy-load `Історія`, `confirm_prog_cancel_cb`, `programs_update_info_from_model`.
2. Перевірити, який саме шлях запускається безпосередньо перед WDT.
3. Лише після підтвердження причини внести мінімальний фікс.

## Статус
- Відкрито.
- Інструментацію додано і повторне відтворення отримано.

## Зібрані докази
- `history_load_async: parsed count=30` і `history_rebuild_list: count=30` відпрацьовують успішно перед зависанням.
- Отже, парсинг `history.json` і первинна побудова списку не є безпосередньою причиною WDT.
- Backtrace після цього веде в `lv_textarea_*`, `keyboard_close`, `programs_update_info_from_model`, що вказує на залишений keyboard/textarea шлях із іншого екрана.

## Поточний висновок
- Найімовірніша причина: keyboard overlay видалявся відкладено (`delete_async`), а textarea/внутрішні LVGL callback-и ще доживали після переходів між екранами і пізніше блокували UI.

## Поточний фікс
- `keyboard_close()` переведено на синхронне видалення keyboard overlay з попереднім обнуленням UI-вказівників.
- Очікую повторної перевірки після прошивки, щоб порівняти `pre-fix` і `post-fix`.

## Додаткові докази
- Новий backtrace знову проходить через `lv_textarea_*`, `buttonmatrix`, `programs_update_info_from_model`, але вже без ознак падіння саме в `history_load_async`.
- Це підтверджує, що `Історія` не є первинною причиною; проблема пов'язана з доживаючими transient overlays / keyboard-popups та важким scroll-path.

## Друга ітерація фіксу
- `action_close_transient_overlays()` тепер закриває не лише програмну клавіатуру, а й `wifi_kb_overlay` та `wifi_setup_popup`.
- `wifi_kb_close_event(NULL)` і `wifi_setup_close_cb(NULL)` видаляють overlay/popup синхронно під час навігації.
- Скрол секції `Історія` перенесено з кореневого `hist_root` у сам `history_list`, щоб зменшити навантаження LVGL на кореневому контейнері.

## Супутня правка UI
- На головному екрані активна помилка тепер показується в полі температури як код помилки, а в нижньому рядку лишається причина, замість простого `--`.
